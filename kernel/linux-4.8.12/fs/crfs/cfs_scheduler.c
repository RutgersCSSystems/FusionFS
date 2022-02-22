#include <linux/devfs.h>
#include <linux/kernel.h>
#include <linux/rbtree.h>
#include <linux/string.h>
#include <linux/time.h>

#define TASK_NUM 4
#define QUEUE_NUM 3

DEFINE_SPINLOCK(rb_lock);
extern spinlock_t rb_lock;

extern struct list_head* g_rd_list;
extern struct mutex g_mutex;

extern struct mutex mm_mutex;
unsigned long get_sched_res(struct crfss_fstruct* rd, int sched_type) {
        switch (sched_type) {
                case SCHED_CPU:
                        return rd->vir_runtime;
                case SCHED_MEM:
                        return rd->mm_budget;
        }
        printk(KERN_ALERT "unrecognized scheduling resource!\n");
        return 0;
}

int update_queue_sched_res(struct crfss_fstruct* rd, nvme_cmdrw_t* cmdrw,
                           int sched_type) {
        long retval = 0;
        switch (sched_type) {
                case SCHED_CPU:
                        rd->vir_runtime += cmdrw->vir_runtime;
                        break;
                case SCHED_MEM:
                        rd->mm_budget += cmdrw->mm_budget;
                        break;
                default:
                        printk(KERN_ALERT
                               "unrecognized scheduling resource!\n");
                        retval = -EFAULT;
                        break;
        }
        return retval;
}

struct crfss_fstruct* rb_search(struct rb_root* root,
                                unsigned long vir_runtime) {
        struct rb_node* node = root->rb_node;

        while (node) {
                struct crfss_fstruct* queue =
                    rb_entry(node, struct crfss_fstruct, rb);
                int64_t result;

                result = vir_runtime - queue->vir_runtime;

                if (result < 0)
                        node = node->rb_left;
                else if (result > 0)
                        node = node->rb_right;
                else
                        return queue;
        }
        return NULL;
}

struct crfss_fstruct* rb_min(struct rb_root* root) {
        struct rb_node* node = root->rb_node;
        struct crfss_fstruct* queue = NULL;
        if (node == NULL) return NULL;
        while (node->rb_left) {
                node = node->rb_left;
        }
        queue = rb_entry(node, struct crfss_fstruct, rb);
        return queue;
}

int rb_insert(struct rb_root* root, struct crfss_fstruct* queue,
              int sched_type) {
        struct rb_node** new = &(root->rb_node), *parent = NULL;

        /* Figure out where to put new node */
        while (*new) {
                struct crfss_fstruct* this =
                    rb_entry(*new, struct crfss_fstruct, rb);
                int64_t result = get_sched_res(queue, sched_type) -
                                 get_sched_res(this, sched_type);

                parent = *new;
                if (result <= 0)
                        new = &((*new)->rb_left);
                else if (result > 0)
                        new = &((*new)->rb_right);
                else
                        return 0;
        }

        /* Add new node and rebalance tree. */
        rb_link_node(&queue->rb, parent, new);
        rb_insert_color(&queue->rb, root);

        return 1;
}

void cfs_init(void) { spin_lock_init(&rb_lock); }

void rb_remove(struct crfss_fstruct* queue) {
        struct dev_thread_struct* target_thread = NULL;
        // Get target device thread from rd
        target_thread = queue->dev_thread_ctx;
        if (target_thread != NULL && !RB_EMPTY_NODE(&queue->rb)) {
                rb_erase(&queue->rb, &target_thread->runtime_tree);
                RB_CLEAR_NODE(&queue->rb);
        }
}

struct crfss_fstruct* pick_next(struct rb_root* root) {
        struct crfss_fstruct* queue = rb_min(root);
        return queue;
}

/* Round-robin scheduling policy combine with CFS */
struct crfss_fstruct* devfs_sched_round_robin_cfs(
    struct dev_thread_struct* dev_thread_ctx, int sched_type) {
        struct crfss_fstruct* rd = NULL;
        u64 min_sched_res = 0;
        u64 sched_res = 0;
        nvme_cmdrw_t* cmdrw = NULL;

        struct crfss_fstruct* queue = pick_next(&dev_thread_ctx->runtime_tree);

        if (!queue || IS_ERR(queue)) {
                min_sched_res = U64_MAX;
        } else {
                min_sched_res = get_sched_res(queue, sched_type);
        }

        if (!dev_thread_ctx) {
                printk(KERN_ALERT "%s:%d Device thread context is NULL \n",
                       __FUNCTION__, __LINE__);
                return NULL;
        }

        /*
         * Case 1 (Fast Path):
         * If current serving is not NULL, picking next available rd queue
         * until reaching the end of global rd list
         */
        if (dev_thread_ctx->current_rd_list &&
            dev_thread_ctx->current_rd_list->next !=
                (&dev_thread_ctx->dummy_rd.list)) {
                rd = list_entry(dev_thread_ctx->current_rd_list,
                                struct crfss_fstruct, list);

                if (!rd || IS_ERR(rd)) {
                        goto rr_slow_path;
                }

                rcu_read_lock();
                list_for_each_entry_continue_rcu(
                    rd, &dev_thread_ctx->dummy_rd.list, list) {
                        if (!rd || IS_ERR(rd)) {
                                goto rr_fast_path_end;
                        }

                        sched_res = get_sched_res(rd, sched_type);

                        /* If it is a read operation, then just schedule it */
                        if (sched_res <= min_sched_res &&
                            test_and_set_bit(0, &rd->state) == DEVFS_RD_IDLE) {
                                dev_thread_ctx->current_rd_list = &rd->list;
                                goto rr_found_rd;
                        }
                }
        rr_fast_path_end:
                rcu_read_unlock();
        }

rr_slow_path:
        /*
         * Case 2 (Slow Path):
         * If current serving is NULL,
         * walk through the global rd list from beginning
         */
        rcu_read_lock();
        list_for_each_entry_rcu(rd, &dev_thread_ctx->dummy_rd.list, list) {
                if (!rd || IS_ERR(rd)) {
                        rd = NULL;
                        goto rr_found_rd;
                }
                sched_res = get_sched_res(rd, sched_type);

                /* If it is a read operation, then just schedule it */
                if (sched_res <= min_sched_res &&
                    test_and_set_bit(0, &rd->state) == DEVFS_RD_IDLE) {
                        dev_thread_ctx->current_rd_list = &rd->list;
                        goto rr_found_rd;
                }
        }
rr_found_rd:
        rcu_read_unlock();

        if (rd != NULL) {
                /* Get the first request in rd queue */
                cmdrw = rd_queue_readtail(rd, sizeof(__u64));

                rb_remove(rd);

                /* Checking queue empty */
                if (cmdrw != NULL) {
                        rd->dev_thread_ctx = dev_thread_ctx;
                        update_queue_sched_res(rd, cmdrw, sched_type);
                        rb_insert(&dev_thread_ctx->runtime_tree, rd,
                                  sched_type);
#ifdef _USE_GMALLOC
                        if (sched_type == SCHED_MEM) {
                                crfss_mutex_lock(&mm_mutex);
                                if (g_mm_counter + cmdrw->mm_budget >
                                    g_mm_budget) {
                                        test_and_clear_bit(0, &rd->state);
                                        rd = NULL;
                                }
                                crfss_mutex_unlock(&mm_mutex);
                        }
#endif
                }
        }
        return rd;
}
EXPORT_SYMBOL(devfs_sched_round_robin_cfs);

