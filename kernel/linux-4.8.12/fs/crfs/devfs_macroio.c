
/*
 * devfs_macroio.c
 *
 * Description: Macro I/O operations
 *
 */
#include <crypto/skcipher.h>
#include <linux/crc32.h>
#include <linux/crypto.h>
#include <linux/devfs.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/iommu.h>
#include <linux/lz4.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/nvme.h>
#include <linux/pagemap.h>
#include <linux/scatterlist.h>
#include <linux/snappy.h>
#include <linux/time.h>
#include <linux/uaccess.h>
#include <linux/vfio.h>
#include <linux/vmalloc.h>
#include <linux/list.h>
#include <linux/list_sort.h>
#include "pmfs.h"
#include "xip.h"
#include "journal.h"

#define KMALLOC_TYPE 0
#define VMALLOC_TYPE 1

#define KB (1024UL)
#define MB (1024 * KB)
#define GB (1024 * MB)

#define _4M 4*1024*1024

/* Linked list */
struct kvnode {
        void *key;
        void *value;
        int keysize;
        int valuesize;
        struct list_head list;
};

LIST_HEAD(llist);

int llist_size = 0;

/* Snappy related */
#define OUTFILE_BASE "/mnt/ram/output_dir/"
static int fidx = 0;

static int batch_idx = 0;

u64 g_mm_budget = 1 * GB;
u64 g_mm_counter = 0;
extern struct mutex mm_mutex;

#ifdef _MACROFS_JOURN
int op_log_fd;
int op_log_data_fd;
struct file *op_log;
struct file *op_log_data;
#endif

/* CRC32 related functions */
const uint32_t kMaskDelta = 0xa282ead8ul;

uint32_t Mask(uint32_t crc) {
        /* Rotate right by 15 bits and add a constant. */
        return ((crc >> 15) | (crc << 17)) + kMaskDelta;
}

uint32_t Unmask(uint32_t masked_crc) {
        uint32_t rot = masked_crc - kMaskDelta;
        return ((rot >> 17) | (rot << 15));
}

void vfio_devfs_put_checksum(struct crfss_fstruct *rd, nvme_cmdrw_t *cmdrw,
                             char *buf) {
        uint32_t crc = 0;

        /* Calculate crc32 of this block */
#ifndef LEVELDB_OFFLOAD_CHECKSUM

#ifdef _DEVFS_MACRO_IO_USE_CRC32
        crc = crc32(0, buf, cmdrw->nlb - 4);
#else
        crc = __crc32c_le(0, buf, cmdrw->nlb - 4);
#endif  //_DEVFS_MACRO_IO_USE_CRC32

#else
        if (cmdrw->meta_pos) {
                crc = crc32(0, buf, cmdrw->nlb - 5);
                crc = crc32(crc, buf + cmdrw->nlb - 5, 1);
        } else {
                crc = crc32(0, buf + 6, 1);
                crc = crc32(crc, buf + 7, cmdrw->nlb - 7);
        }
        crc = Mask(crc);
#endif  // LEVELDB_OFFLOAD_CHECKSUM

#ifndef LEVELDB_OFFLOAD_CHECKSUM
        /* write checksum value to end of buf */
        memcpy(buf + cmdrw->nlb - 4, (void *)&crc, sizeof(__u32));
#else
        if (cmdrw->meta_pos) {
                memcpy(buf + cmdrw->nlb - 4, (void *)&crc, sizeof(__u32));
        } else {
                memcpy(buf, (void *)&crc, sizeof(__u32));
        }
#endif  // LEVELDB_OFFLOAD_CHECKSUM

#ifdef _MACROFS_JOURN
        printk(KERN_ALERT "chm written = %x", crc);
        /* Add checksum log entry */
        crfs_macro_add_log_entry(rd, &crc, sizeof(__u32), LE_DATA, 0);

        /* Set commit bit in place for first micro op -> checksum */
        crfs_macro_commit_micro_trans(rd, 0);
#endif
}

int vfio_devfs_get_checksum(struct crfss_fstruct *rd, nvme_cmdrw_t *cmdrw,
                            char *buf) {
        int retval = 0;
        uint32_t crc = 0;
        uint32_t actual = 0;

#ifdef LEVELDB_OFFLOAD_CHECKSUM
        uint32_t mask_crc = 0;
#endif

#ifndef LEVELDB_OFFLOAD_CHECKSUM

        /* Get stored CRC value */
        memcpy(&crc, buf + cmdrw->nlb - 4, sizeof(__u32));

        /* Calculate actual CRC value */
#ifdef _DEVFS_MACRO_IO_USE_CRC32
        actual = crc32(0, buf, cmdrw->nlb - 4);
#else
        actual = __crc32c_le(0, buf, cmdrw->nlb - 4);
#endif  //_DEVFS_MACRO_IO_USE_CRC32

#else
        if (cmdrw->meta_pos) {
                memcpy(&mask_crc, buf + cmdrw->nlb - 4, sizeof(mask_crc));
                crc = Unmask(mask_crc);
                actual = crc32(0, buf, cmdrw->nlb - 4);
        } else {
                const uint32_t a = (uint32_t)(((char *)buf)[4]) & 0xff;
                const uint32_t b = (uint32_t)(((char *)buf)[5]) & 0xff;
                const uint32_t length = a | (b << 8);
                memcpy(&mask_crc, buf, sizeof(mask_crc));
                crc = Unmask(mask_crc);
                actual = crc32(0, buf + 6, length + 1);
        }
#endif  // LEVELDB_OFFLOAD_CHECKSUM

        /* Compare CRC value */
        if (crc != actual) {
                printk(KERN_ALERT "checksum verify failed!\n");
                printk(KERN_ALERT
                       "DEBUG: %s:%d, meta_pos: %u, crc: %d, actual: %d \n",
                       __FUNCTION__, __LINE__, cmdrw->meta_pos, crc, actual);

                printk(KERN_ALERT "DEBUG: %s:%d, crc: %d, slba: %lld, nlb: %lld \n",
                       __FUNCTION__, __LINE__, crc, cmdrw->slba, cmdrw->nlb);
                retval = -EFAULT;
        }
        return retval;
}

long vfio_devfs_macro_read_checksum(struct crfss_fstruct *rd,
                                    nvme_cmdrw_t *cmdrw,
                                    nvme_cmdrw_t *cmdrw_after, char *buf) {
        long retval = 0;

        /* Read the block from storage first */
        cmdrw_after->common.opc = nvme_cmd_read;
        if (cmdrw->slba == DEVFS_INVALID_SLBA)
                retval = vfio_crfss_io_read(rd, cmdrw_after, 1);
        else
                retval = vfio_crfss_io_read(rd, cmdrw_after, 0);

        /* Now the block should be in buffe, then get checksum from block and
         * verify */
        if (vfio_devfs_get_checksum(rd, cmdrw, buf)) {
                retval = -EFAULT;
                goto macro_io_read_checksum_exit;
        }

        if (copy_to_user((void __user *)cmdrw->common.prp2, buf, retval)) {
                printk(KERN_ALERT "DEBUG: Failed %s:%d \n", __FUNCTION__,
                       __LINE__);
                retval = -EFAULT;
                goto macro_io_read_checksum_exit;
        }

macro_io_read_checksum_exit:
        return retval;
}

long vfio_devfs_macro_write_checksum(struct crfss_fstruct *rd,
                                     nvme_cmdrw_t *cmdrw,
                                     nvme_cmdrw_t *cmdrw_after, char *buf) {
        long retval = 0;
        char *p = (__force char __user *)buf;

#ifdef _MACROFS_JOURN
        struct inode *inode = rd->fp->f_inode;
        struct super_block *sb = inode->i_sb;
        struct crfs_inode *pi = crfs_get_inode(sb, inode->i_ino);

        /* Initialize a new transaction with 2 micro ops */
        crfs_macro_transaction_init(rd, cmdrw, 2);

        /* Add meta-data log entry */
        crfs_macro_add_log_entry(rd, pi, MAX_DATA_PER_LENTRY, LE_DATA, -1);
#endif

        cmdrw_after->common.opc = nvme_cmd_read;
        vfio_crfss_io_read(rd, cmdrw_after, DEVFS_MACRO_IO_READ);

        /* Write modified cotent to this block */
        if (copy_from_user(buf, (void __user *)cmdrw->common.prp2,
                           cmdrw->nlb)) {
                printk(KERN_ALERT "DEBUG: Failed %s:%d \n", __FUNCTION__,
                       __LINE__);
                retval = -EFAULT;
                goto macro_io_write_checksum_exit;
        }

        /* Calculate crc32 of this block */
        vfio_devfs_put_checksum(rd, cmdrw, buf);

        /* Copy cmdrw to cmdrw_after */
        memcpy(cmdrw_after, cmdrw, sizeof(nvme_cmdrw_t));
        cmdrw_after->common.prp2 = (u64)p;
        cmdrw_after->blk_addr = (u64)p;

        /* Write entire block back */
        cmdrw_after->common.opc = nvme_cmd_write;
        retval = vfio_crfss_io_write(rd, cmdrw_after);

#ifdef _MACROFS_JOURN
        /* Set commit bit in place for second micro op -> data block */
        crfs_macro_commit_micro_trans(rd, 1);
#endif

macro_io_write_checksum_exit:
        return retval;
}

long vfio_devfs_macro_append_checksum(struct crfss_fstruct *rd,
                                      nvme_cmdrw_t *cmdrw,
                                      nvme_cmdrw_t *cmdrw_after, char *buf) {
        long retval = 0;
        char *p = (__force char __user *)buf;

#ifdef _MACROFS_JOURN
        struct inode *inode = rd->fp->f_inode;
        struct super_block *sb = inode->i_sb;
        struct crfs_inode *pi = crfs_get_inode(sb, inode->i_ino);

        /* Initialize a new transaction with 2 micro ops */
        crfs_macro_transaction_init(rd, cmdrw, 2);

        /* Add meta-data log entry */
        crfs_macro_add_log_entry(rd, pi, MAX_DATA_PER_LENTRY, LE_DATA, -1);
#endif

        /* Copy the buffer from user to kernel */
        if (copy_from_user(buf, (void __user *)cmdrw->common.prp2,
                           cmdrw->nlb)) {
                /* if (copy_from_user(buf, (void __user *)cmdrw->blk_addr,
                 * cmdrw->nlb)) { */
                printk(KERN_ALERT "DEBUG: Failed %s:%d \n", __FUNCTION__,
                       __LINE__);
                retval = -EFAULT;
                goto macro_io_append_checksum_exit;
        }

        /* Calculate crc32 of this block */
        vfio_devfs_put_checksum(rd, cmdrw, buf);

        /* Copy cmdrw to cmdrw_after */
        memcpy(cmdrw_after, cmdrw, sizeof(nvme_cmdrw_t));
        cmdrw_after->common.prp2 = (u64)p;
        cmdrw_after->blk_addr = (u64)p;

        /* Write entire block back */
        retval = vfio_crfss_io_append(rd, cmdrw_after);

#ifdef _MACROFS_JOURN
        /* Set commit bit in place for second micro op -> data block */
        crfs_macro_commit_micro_trans(rd, 1);
#endif

macro_io_append_checksum_exit:
        return retval;
}

long vfio_devfs_macro_io_readmodifywrite(struct crfss_fstruct *rd,
                                         nvme_cmdrw_t *cmdrw,
                                         nvme_cmdrw_t *cmdrw_after, char *buf) {
        long retval = 0;
        char *p = (__force char __user *)buf;

        /* Read data block internally */
        cmdrw_after->common.opc = nvme_cmd_read;
        vfio_crfss_io_read(rd, cmdrw_after, DEVFS_MACRO_IO_READ);

        /* Write modified cotent to this block */
        if (copy_from_user(buf, (void __user *)cmdrw->common.prp2,
                           cmdrw->nlb)) {
                printk(KERN_ALERT "DEBUG: Failed %s:%d \n", __FUNCTION__,
                       __LINE__);
                retval = -EFAULT;
                goto macro_io_readmodifywrite_exit;
        }

        /* Copy cmdrw to cmdrw_after */
        memcpy(cmdrw_after, cmdrw, sizeof(nvme_cmdrw_t));
        cmdrw_after->common.prp2 = (u64)p;
        cmdrw_after->blk_addr = (u64)p;

        /* Write entire block back */
        cmdrw_after->common.opc = nvme_cmd_write;
        retval = vfio_crfss_io_write(rd, cmdrw_after);

macro_io_readmodifywrite_exit:
        return retval;
}

long vfio_devfs_macro_write_checksum_batch(struct crfss_fstruct *rd,
                                           nvme_cmdrw_t *cmdrw,
                                           nvme_cmdrw_t *cmdrw_after,
                                           char *buf) {
        long retval = 0;
        char *p = (__force char __user *)buf;
        int i = 0, num_op = 0;
        num_op = cmdrw->common.num_op;

        for (i = 0; i < num_op; ++i) {
                /* Copy the buffer from user to kernel */
                if (copy_from_user(buf, (void __user *)cmdrw->common.prp_vec[i],
                                   cmdrw->param_vec[i].data_param.nlb)) {
                        printk(KERN_ALERT "DEBUG: Failed %s:%d \n",
                               __FUNCTION__, __LINE__);
                        retval = -EFAULT;
                        goto macro_io_append_checksum_exit;
                }

                /* Calculate crc32 of this block */
                vfio_devfs_put_checksum(rd, cmdrw, buf);

                /* Copy cmdrw to cmdrw_after */
                cmdrw_after->common.prp2 = (u64)p;
                cmdrw_after->blk_addr = (u64)p;
                cmdrw_after->slba = cmdrw->param_vec[i].data_param.slba;
                cmdrw_after->nlb = cmdrw->param_vec[i].data_param.nlb;
                /* Write entire block back */
                retval = vfio_crfss_io_write(rd, cmdrw_after);
        }

macro_io_append_checksum_exit:
        return retval;
}

long vfio_devfs_macro_io_readmodifywrite_batch(struct crfss_fstruct *rd,
                                               nvme_cmdrw_t *cmdrw,
                                               nvme_cmdrw_t *cmdrw_after,
                                               char *buf) {
        long retval = 0;
        char *p = (__force char __user *)buf;
        int i = 0, num_op = 0;
        num_op = cmdrw->common.num_op;
        for (i = 0; i < num_op; ++i) {
                cmdrw_after->common.opc = nvme_cmd_read;
                cmdrw_after->slba = cmdrw->param_vec[i].data_param.slba;
                cmdrw_after->nlb = cmdrw->param_vec[i].data_param.nlb;
                vfio_crfss_io_read(rd, cmdrw_after, DEVFS_MACRO_IO_READ);

                /* Write modified cotent to this block */
                if (copy_from_user(buf, (void __user *)cmdrw->common.prp_vec[i],
                                   cmdrw->param_vec[i].data_param.nlb)) {
                        printk(KERN_ALERT "DEBUG: Failed %s:%d \n",
                               __FUNCTION__, __LINE__);
                        retval = -EFAULT;
                        goto macro_io_readmodifywrite_exit;
                }

                cmdrw_after->common.prp2 = (u64)p;
                cmdrw_after->blk_addr = (u64)p;
                cmdrw_after->common.opc = nvme_cmd_write;
                retval = vfio_crfss_io_write(rd, cmdrw_after);
        }

macro_io_readmodifywrite_exit:
        return retval;
}

long vfio_devfs_macro_io_openreadclose(struct crfss_fstruct *rd,
                                       nvme_cmdrw_t *cmdrw,
                                       nvme_cmdrw_t *cmdrw_after, char *buf) {
        long retval = 0;
        int newfd;
        char outfile[256];
        struct file *fp = NULL;
        struct fd f;

        if (copy_from_user(outfile, (char __user *)cmdrw->common.prp_vec[0],
                           cmdrw->param_vec[0].data_param.nlb)) {
                printk(KERN_ALERT "DEBUG: Failed %s:%d \n", __FUNCTION__,
                       __LINE__);
                goto macro_io_openwriteclose_exit;
                ;
        }
        outfile[cmdrw->param_vec[0].data_param.nlb] = '\0';
        newfd = vfio_creatfile_inkernel(outfile, (O_CREAT | O_RDWR),
                                        (umode_t)0666, 0);
        if (newfd < 0) {
                printk(KERN_ALERT
                       "VFIO_DEVFS_CREATFILE_CMD for %s  "
                       "failed fd %d \n",
                       outfile, newfd);
                goto macro_io_openwriteclose_exit;
        }

        f = fdget(newfd);
        fp = f.file;
        if (!fp) {
                printk(KERN_ALERT "%s, %d failed to get file pointer \n",
                       __FUNCTION__, __LINE__);
                retval = EFAULT;
                goto macro_io_openwriteclose_exit;
        }
        BUG_ON(!fp->isdevfs);
        fdput(f);

        /* Copy cmdrw to cmdrw_after */
        memcpy(cmdrw_after, cmdrw, sizeof(nvme_cmdrw_t));
        cmdrw_after->common.prp2 = (u64)cmdrw->common.prp2;
        cmdrw_after->blk_addr = (u64)cmdrw->common.prp2;

        /* Write entire block back */
        cmdrw_after->common.opc = nvme_cmd_read;
        retval = vfio_crfss_io_read_inkernel(fp, cmdrw_after, 0);

        /* Close the file */
        if (crfss_close_inkernel(fp, newfd)) {
                printk(KERN_ALERT "%s:%d devfs_close failed\n", __FUNCTION__,
                       __LINE__);
                goto macro_io_openwriteclose_exit;
        }

macro_io_openwriteclose_exit:
        return retval;
}

long vfio_devfs_macro_io_openwriteclose(struct crfss_fstruct *rd,
                                        nvme_cmdrw_t *cmdrw,
                                        nvme_cmdrw_t *cmdrw_after, char *buf) {
        long retval = 0;
        int newfd;
        char outfile[256];
        struct file *fp = NULL;
        struct fd f;

        if (copy_from_user(outfile, (char __user *)cmdrw->common.prp_vec[0],
                           cmdrw->param_vec[0].data_param.nlb)) {
                printk(KERN_ALERT "DEBUG: Failed %s:%d \n", __FUNCTION__,
                       __LINE__);
                goto macro_io_openwriteclose_exit;
                ;
        }
        outfile[cmdrw->param_vec[0].data_param.nlb] = '\0';
        newfd = vfio_creatfile_inkernel(outfile, (O_CREAT | O_RDWR),
                                        (umode_t)0666, 0);
        if (newfd < 0) {
                printk(KERN_ALERT
                       "VFIO_DEVFS_CREATFILE_CMD for %s  "
                       "failed fd %d \n",
                       outfile, newfd);
                goto macro_io_openwriteclose_exit;
        }

        f = fdget(newfd);
        fp = f.file;
        if (!fp) {
                printk(KERN_ALERT "%s, %d failed to get file pointer \n",
                       __FUNCTION__, __LINE__);
                retval = EFAULT;
                goto macro_io_openwriteclose_exit;
        }
        BUG_ON(!fp->isdevfs);
        fdput(f);

        /* Copy cmdrw to cmdrw_after */
        memcpy(cmdrw_after, cmdrw, sizeof(nvme_cmdrw_t));
        cmdrw_after->common.prp2 = (u64)cmdrw->common.prp2;
        cmdrw_after->blk_addr = (u64)cmdrw->common.prp2;

        /* Write entire block back */
        cmdrw_after->common.opc = nvme_cmd_write;
        retval = vfio_crfss_io_write_inkernel(fp, cmdrw_after);

        /* Close the file */
        if (crfss_close_inkernel(fp, newfd)) {
                printk(KERN_ALERT "%s:%d devfs_close failed\n", __FUNCTION__,
                       __LINE__);
                goto macro_io_openwriteclose_exit;
        }

macro_io_openwriteclose_exit:
        return retval;
}

long vfio_devfs_macro_io_openwriteclose_batch(struct crfss_fstruct *rd,
                                              nvme_cmdrw_t *cmdrw,
                                              nvme_cmdrw_t *cmdrw_after,
                                              char *buf) {
        long retval = 0;
        int newfd;
        char outfile[256];
        char outbase[256];
        char fileno[64];
        struct file *fp = NULL;
        struct fd f;

        int i = 0, num_op = 0;
        num_op = cmdrw->common.num_op;
        if (copy_from_user(outbase, (char __user *)cmdrw->common.prp2,
                           cmdrw->nlb)) {
                printk(KERN_ALERT "DEBUG: Failed %s:%d \n", __FUNCTION__,
                       __LINE__);
                goto macro_io_openwriteclose_exit;
                ;
        }

        outbase[cmdrw->nlb] = '\0';
        for (i = 0; i < num_op; ++i) {
                strcpy(outfile, outbase);

                snprintf(fileno, 64, "%d", batch_idx);
                strcat(outfile, fileno);
                atomic_fetch_add(1, (atomic_t *)&batch_idx);
                newfd = vfio_creatfile_inkernel(outfile, (O_CREAT | O_RDWR),
                                                (umode_t)0666, 0);

                if (newfd < 0) {
                        printk(KERN_ALERT
                               "VFIO_DEVFS_CREATFILE_CMD for %s  "
                               "failed fd %d \n",
                               outfile, newfd);
                        goto macro_io_openwriteclose_exit;
                }

                f = fdget(newfd);
                fp = f.file;
                if (!fp) {
                        printk(KERN_ALERT
                               "%s, %d failed to get file pointer \n",
                               __FUNCTION__, __LINE__);
                        retval = EFAULT;
                        goto macro_io_openwriteclose_exit;
                }
                BUG_ON(!fp->isdevfs);
                fdput(f);

                /* copy cmdrw to cmdrw_after */
                memcpy(cmdrw_after, cmdrw, sizeof(nvme_cmdrw_t));
                cmdrw_after->common.prp2 = (u64)cmdrw->common.prp_vec[i];
                cmdrw_after->blk_addr = (u64)cmdrw->common.prp_vec[i];
                cmdrw_after->slba = cmdrw->param_vec[i].data_param.slba;
                cmdrw_after->nlb = cmdrw->param_vec[i].data_param.nlb;

                /* write entire block back */
                cmdrw_after->common.opc = nvme_cmd_write;
                retval = vfio_crfss_io_write_inkernel(fp, cmdrw_after);

                /* close the file */
                if (crfss_close_inkernel(fp, newfd)) {
                        printk(KERN_ALERT "%s:%d devfs_close failed\n",
                               __FUNCTION__, __LINE__);
                        goto macro_io_openwriteclose_exit;
                }
        }

macro_io_openwriteclose_exit:
        return retval;
}

long vfio_devfs_macro_io_readmodifyappend(struct crfss_fstruct *rd,
                                          nvme_cmdrw_t *cmdrw,
                                          nvme_cmdrw_t *cmdrw_after,
                                          char *buf) {
        long retval = 0;
        char *p = (__force char __user *)buf;

        /* read data block internally */
        cmdrw_after->common.opc = nvme_cmd_read;
        vfio_crfss_io_read(rd, cmdrw_after, DEVFS_READ_APPEND);

        /* now the block should be in buffer */

        /* write modified cotent to this block */
        if (copy_from_user(buf, (void __user *)cmdrw->common.prp2,
                           cmdrw->nlb)) {
                printk(KERN_ALERT "DEBUG: Failed %s:%d \n", __FUNCTION__,
                       __LINE__);
                retval = -EFAULT;
                goto macro_io_readmodifyappend_exit;
        }

        /* copy cmdrw to cmdrw_after */
        memcpy(cmdrw_after, cmdrw, sizeof(nvme_cmdrw_t));
        cmdrw_after->common.prp2 = (u64)p;
        cmdrw_after->blk_addr = (u64)p;

        /* write entire block back */
        cmdrw_after->common.opc = nvme_cmd_write;
        retval = vfio_crfss_io_append(rd, cmdrw_after);

macro_io_readmodifyappend_exit:
        return retval;
}

long vfio_devfs_macro_io_readappend(struct crfss_fstruct *rd,
                                    nvme_cmdrw_t *cmdrw,
                                    nvme_cmdrw_t *cmdrw_after, char *buf) {
        long retval = 0;
        char *p = (__force char __user *)buf;
        char *read_buf = NULL;
        loff_t i_size = rd->fp->f_inode->i_size;

        if (i_size > 0) {
                /* Allocate in-kernel buffer */
                read_buf = vmalloc(rd->fp->f_inode->i_size);
                if (!read_buf) {
                        printk(KERN_ALERT
                               "DEBUG: vmalloc failed %s:%d, count: %lld \n",
                               __FUNCTION__, __LINE__, rd->fp->f_inode->i_size);
                        retval = -EFAULT;
                        goto macro_io_readappend_exit;
                }

                /* read data block internally */
                cmdrw_after->common.opc = nvme_cmd_read;
                cmdrw_after->common.prp2 = (u64) read_buf;
                cmdrw_after->slba = 0;
                cmdrw_after->nlb = rd->fpos;
                vfio_crfss_io_read(rd, cmdrw_after, 0);
        }
        /* copy user data to this block */
        if (copy_from_user(buf, (void __user *)cmdrw->common.prp2,
                           cmdrw->nlb)) {
                printk(KERN_ALERT "DEBUG: Failed %s:%d \n", __FUNCTION__,
                       __LINE__);
                retval = -EFAULT;
                goto macro_io_readappend_exit;
        }

        /* copy cmdrw to cmdrw_after */
        memcpy(cmdrw_after, cmdrw, sizeof(nvme_cmdrw_t));
        cmdrw_after->common.prp2 = (u64)p;
        cmdrw_after->blk_addr = (u64)p;

        /* write entire block back */
        cmdrw_after->common.opc = nvme_cmd_write;
        retval = vfio_crfss_io_append(rd, cmdrw_after);

macro_io_readappend_exit:
        if (read_buf) {
                vfree(read_buf);
        }
        return retval;
}

long vfio_devfs_macro_io_compresswrite(struct crfss_fstruct *rd,
                                       nvme_cmdrw_t *cmdrw, unsigned int fidx) {
        long retval = 0;
        struct file *in_fp = NULL;
        struct file *out_fp = NULL;
        size_t isize = 0;
        char *outbuf = NULL, *inbuf = NULL;
        const char __user *compressd = NULL;
        ssize_t rdbytes = 0;
        loff_t fpos = 0; /*Always read from byte 0 */
        size_t compress_len;
        int err = 0;
        int out_fd = -1;
        int in_fd = -1;
        char outfile[64];
        char infile[64];
        char fileno[64];
        struct dev_thread_struct *target_thread = NULL;
        struct fd f;

        int inbuf_kmalloc = 0;
        int outbuf_kmalloc = 0;

        target_thread = rd->dev_thread_ctx;
        if (!target_thread) {
                printk(KERN_ALERT "DEBUG: no target_thread! \n");
        }
        /*
         * if snappy compression environment variable is not set,
         * then initialize it.
         */
        if (!target_thread->g_snappy_init) {
                if (snappy_init_env(&target_thread->g_snappy_env)) {
                        printk(KERN_ALERT "DEBUG: Failed %s:%d \n",
                               __FUNCTION__, __LINE__);
                        goto err_perf_comprss;
                }

                target_thread->g_snappy_init = 1;

                /* printk(KERN_ALERT "DEBUG: Snappy init successfully \n"); */
        }

        /* copy input filepath */
        if (copy_from_user(infile, (char __user *)cmdrw->common.prp_vec[0],
                           cmdrw->param_vec[0].data_param.nlb)) {
                printk(KERN_ALERT "DEBUG: Failed %s:%d \n", __FUNCTION__,
                       __LINE__);
                goto err_perf_comprss;
        }
        infile[cmdrw->param_vec[0].data_param.nlb] = '\0';

        /* open input file */
        in_fd = vfio_creatfile_inkernel(infile, O_RDWR, (umode_t)0666, 0);

        if (in_fd < 0) {
                printk(KERN_ALERT
                       "VFIO_DEVFS_CREATFILE_CMD for %s  "
                       "failed fd %d \n",
                       infile, in_fd);
                goto err_perf_comprss;
        }

        f = fdget(in_fd);
        in_fp = f.file;
        if (!in_fp) {
                printk(KERN_ALERT "%s, %d failed to get file pointer \n",
                       __FUNCTION__, __LINE__);
                retval = EFAULT;
                goto err_perf_comprss;
        }
        BUG_ON(!in_fp->isdevfs);
        fdput(f);

        isize = in_fp->f_inode->i_size;
        if (!isize) {
                printk(KERN_ALERT "%s, %d file size %lu\n", __FUNCTION__,
                       __LINE__, isize);
                goto err_perf_comprss;
        }

        /* allocate kernel buffer to store the input data */

#ifdef _USE_KMALLOC
        inbuf = NULL;
        inbuf = kmalloc(isize, GFP_USER);
        inbuf_kmalloc = 1;
#endif
        if (!inbuf) {
                inbuf = vmalloc(isize);
                inbuf_kmalloc = 0;
        }
        if (!inbuf) {
                printk(KERN_ALERT "%s, %d vmalloc fail\n", __FUNCTION__,
                       __LINE__);
                goto err_perf_comprss;
        }

#ifndef _DEVFS_XIP_IO
        rdbytes = crfss_read(in_fp, inbuf, isize, &fpos);
#else
        rdbytes = crfs_xip_file_read(in_fp, (char __user *)inbuf, isize, &fpos);
#endif
        if (rdbytes != isize) {
                printk(KERN_ALERT
                       "%s, %d devfs_read fail read %zu memloc %lu\n",
                       __FUNCTION__, __LINE__, rdbytes, (unsigned long)inbuf);
                goto err_perf_comprss;
        }

        /* printk(KERN_ALERT "file size: %lu, pos: %lu, read bytes: %lu \n",
                isize, fp->f_pos, rdbytes); */

        /*
         * allocate kernel buffer to store the compressed data,
         * compressed file might have larger size than origin
         * file
         */
#ifdef _USE_KMALLOC
        outbuf = NULL;
        outbuf = kmalloc(isize * 2, GFP_USER);
        outbuf_kmalloc = 1;
#endif

        if (!outbuf) {
                outbuf = vmalloc(isize * 2);
                outbuf_kmalloc = 0;
        }

        if (!outbuf) {
                printk(KERN_ALERT "%s, %d vmalloc fail\n", __FUNCTION__,
                       __LINE__);
                goto err_perf_comprss;
        }

        if ((err = snappy_compress(&target_thread->g_snappy_env,
                                   (const char *)inbuf, isize, outbuf,
                                   &compress_len)) != 0) {
                printk(KERN_ALERT "%s, %d compress failed\n", __FUNCTION__,
                       __LINE__);
        }

        compressd = outbuf;

        /* create and open a new output file */
        strcpy(outfile, OUTFILE_BASE);
        snprintf(fileno, 64, "%d", fidx);
        strcat(outfile, fileno);

        out_fd = vfio_creatfile_inkernel(outfile, (O_CREAT | O_RDWR),
                                         (umode_t)0666, 0);
        if (out_fd < 0) {
                printk(KERN_ALERT
                       "VFIO_DEVFS_CREATFILE_CMD for %s  "
                       "failed fd %d \n",
                       outfile, out_fd);
                goto err_perf_comprss;
        }

        f = fdget(out_fd);
        out_fp = f.file;
        if (!out_fp) {
                printk(KERN_ALERT "%s, %d failed to get file pointer \n",
                       __FUNCTION__, __LINE__);
                retval = EFAULT;
                goto err_perf_comprss;
        }
        BUG_ON(!out_fp->isdevfs);
        fdput(f);

        cmdrw->nlb = compress_len;
        cmdrw->common.opc = nvme_cmd_append;
        cmdrw->common.prp2 = (u64)compressd;
        cmdrw->blk_addr = (u64)compressd;

        /* Write the compressed data to output file */
        rdbytes = vfio_crfss_io_append_inkernel(out_fp, cmdrw);
        // rdbytes = compress_len;

        if (rdbytes != compress_len) {
                printk(KERN_ALERT "%s:%d perform_write failed %lu\n",
                       __FUNCTION__, __LINE__, rdbytes);
                goto err_perf_comprss;
        }

        /* close the output file */
        if (crfss_close_inkernel(out_fp, out_fd)) {
                printk(KERN_ALERT "%s:%d devfs_close failed\n", __FUNCTION__,
                       __LINE__);
                goto err_perf_comprss;
        }

        /* close the input file */
        if (crfss_close_inkernel(in_fp, in_fd)) {
                printk(KERN_ALERT "%s:%d devfs_close failed\n", __FUNCTION__,
                       __LINE__);
                goto err_perf_comprss;
        }

err_perf_comprss:

        if (inbuf) {
                if (inbuf_kmalloc)
                        kfree(inbuf);
                else
                        vfree(inbuf);
        }

        if (outbuf) {
                if (outbuf_kmalloc)
                        kfree(outbuf);
                else
                        vfree(outbuf);
        }

        if (err) {
                printk(KERN_ALERT "%s:%d Snappy failed \n", __FUNCTION__,
                       __LINE__);
                return -1;
        }
        return rdbytes;
}
/*
 * Macro I/O handler for compound operations
 */
long vfio_devfs_macro_io_handler(struct crfss_fstruct *rd,
                                 nvme_cmdrw_t *cmdrw) {
        long retval = 0;
        char *buf = NULL;
        char *p = NULL;
        nvme_cmdrw_t *cmdrw_after = NULL;
        mm_segment_t oldfs = get_fs();

        if (cmdrw->common.opc != nvme_cmd_compress_write &&
            cmdrw->common.opc != nvme_cmd_open_write_close &&
            cmdrw->common.opc != nvme_cmd_open_pread_close) {
                /* Allocate in-kernel buffer */
                buf = kmalloc(cmdrw->nlb, GFP_KERNEL);
                
                if (!buf) {
                        printk(KERN_ALERT "DEBUG: kmalloc failed %s:%d \n",
                               __FUNCTION__, __LINE__);
                        retval = -EFAULT;
                        goto macro_io_handler_exit;
                }
                p = (__force char __user *)buf;
        }

        /* Copy cmdrw to cmdrw_after */
        cmdrw_after = kmalloc(sizeof(nvme_cmdrw_t), GFP_KERNEL);
        if (!cmdrw_after) {
                printk(KERN_ALERT "DEBUG: kmalloc failed %s:%d \n",
                       __FUNCTION__, __LINE__);
                retval = -EFAULT;
                goto macro_io_handler_exit;
        }
        memcpy(cmdrw_after, cmdrw, sizeof(nvme_cmdrw_t));
        cmdrw_after->common.prp2 = (u64)p;

        /* Set free segment to fake a kernel pointer as user pointer */
        set_fs(KERNEL_DS);

        if (cmdrw->common.opc == nvme_cmd_read_chksm) {
                /* checksum read */
                retval =
                    vfio_devfs_macro_read_checksum(rd, cmdrw, cmdrw_after, buf);
                if (retval == -EFAULT) {
                        goto macro_io_handler_exit;
                }
        } else if (cmdrw->common.opc == nvme_cmd_write_chksm) {
                /* checksum write */
                retval = vfio_devfs_macro_write_checksum(rd, cmdrw, cmdrw_after,
                                                         buf);
                if (retval == -EFAULT) {
                        goto macro_io_handler_exit;
                }
        } else if (cmdrw->common.opc == nvme_cmd_append_chksm) {
                /* checksum append */
                retval = vfio_devfs_macro_append_checksum(rd, cmdrw,
                                                          cmdrw_after, buf);
                if (retval == -EFAULT) {
                        goto macro_io_handler_exit;
                }
        } else if (cmdrw->common.opc == nvme_cmd_read_modify_write) {
                /* read modify write */
                retval = vfio_devfs_macro_io_readmodifywrite(rd, cmdrw,
                                                             cmdrw_after, buf);
                if (retval == -EFAULT) {
                        goto macro_io_handler_exit;
                }
        } else if (cmdrw->common.opc == nvme_cmd_read_modify_write_batch) {
                /* read modify write */
                retval = vfio_devfs_macro_io_readmodifywrite_batch(
                    rd, cmdrw, cmdrw_after, buf);
                if (retval == -EFAULT) {
                        goto macro_io_handler_exit;
                }
        } else if (cmdrw->common.opc == nvme_cmd_write_chksm_batch) {
                /* write checksum batch */
                retval = vfio_devfs_macro_write_checksum_batch(
                    rd, cmdrw, cmdrw_after, buf);
                if (retval == -EFAULT) {
                        goto macro_io_handler_exit;
                }
        } else if (cmdrw->common.opc == nvme_cmd_read_modify_append) {
                /* read modify write */
                retval = vfio_devfs_macro_io_readmodifyappend(rd, cmdrw,
                                                              cmdrw_after, buf);
                if (retval == -EFAULT) {
                        goto macro_io_handler_exit;
                }
        } else if (cmdrw->common.opc == nvme_cmd_read_append) {
                retval =
                    vfio_devfs_macro_io_readappend(rd, cmdrw, cmdrw_after, buf);
                if (retval == -EFAULT) {
                        goto macro_io_handler_exit;
                }
        } else if (cmdrw->common.opc == nvme_cmd_compress_write) {
                /* compress write */
                retval = vfio_devfs_macro_io_compresswrite(rd, cmdrw, fidx);
                atomic_fetch_add(1, (atomic_t *) &fidx);
                if (retval == -EFAULT) {
                        goto macro_io_handler_exit;
                }
        } else if (cmdrw->common.opc == nvme_cmd_open_write_close) {
                retval = vfio_devfs_macro_io_openwriteclose(rd, cmdrw,
                                                            cmdrw_after, buf);
                if (retval == -EFAULT) {
                        goto macro_io_handler_exit;
                }
        } else if (cmdrw->common.opc == nvme_cmd_open_write_close_batch) {
                retval = vfio_devfs_macro_io_openwriteclose_batch(
                    rd, cmdrw, cmdrw_after, buf);
                if (retval == -EFAULT) {
                        goto macro_io_handler_exit;
                }
        } else if (cmdrw->common.opc == nvme_cmd_open_pread_close) {
                retval = vfio_devfs_macro_io_openreadclose(rd, cmdrw,
                                                           cmdrw_after, buf);
                if (retval == -EFAULT) {
                        goto macro_io_handler_exit;
                }

        } else {
                printk(KERN_ALERT "invalid compound opcode %x | %s:%d\n",
                       cmdrw->common.opc, __FUNCTION__, __LINE__);
                retval = -EFAULT;
        }

macro_io_handler_exit:
        if (buf) {
                kfree(buf);
        }

        if (cmdrw_after) {
                kfree(cmdrw_after);
        }

        /* set fs register back */
        set_fs(oldfs);
        return retval;
}

struct crfss_fstruct *vfio_process_micro_op_open(nvme_cmdrw_t *cmdrw,
                                                 int vec_idx) {
        int i = vec_idx;
        int newfd;
        struct vfio_crfss_creatfp_cmd map;
        char outfile[256];
        struct crfss_fstruct *newrd = NULL;

        if (copy_from_user(outfile, (char __user *)cmdrw->common.prp_vec[i],
                           cmdrw->param_vec[i].data_param.nlb)) {
                printk(KERN_ALERT "DEBUG: Failed %s:%d \n", __FUNCTION__,
                       __LINE__);
                goto micro_op_open_err;
        }
        outfile[cmdrw->param_vec[i].data_param.nlb] = '\0';

        newfd = vfio_creatfile_inkernel(outfile, (O_CREAT | O_RDWR), 0666, 0);
        if (newfd < 0) {
                printk(KERN_ALERT
                       "VFIO_DEVFS_CREATFILE_CMD for %s  "
                       "failed fd %d \n",
                       map.fname, map.fd);
                goto micro_op_open_err;
        }
        newrd = fd_to_queuebuf(newfd);
        if (!newrd) {
                printk(KERN_ALERT "%s:%d fd_to_queuebuf failed %d\n",
                       __FUNCTION__, __LINE__, newfd);
                goto micro_op_open_err;
        }

        return newrd;

micro_op_open_err:
        return NULL;
}

long vfio_process_micro_op_read(struct crfss_fstruct *rd, nvme_cmdrw_t *cmdrw,
                                nvme_cmdrw_t *cmdrw_mop, int vec_idx,
                                char *buf) {
        long retval = 0;
        int i = vec_idx;
        uint8_t isappend = 0;
        char *p = NULL;

        /* setup microp op cmdrw */
        cmdrw_mop->common.opc = nvme_cmd_read;

        p = (__force char __user *)buf;
        cmdrw_mop->common.prp2 = (uint64_t)p;

        cmdrw_mop->slba = cmdrw->param_vec[i].data_param.slba;
        cmdrw_mop->nlb = cmdrw->param_vec[i].data_param.nlb;

        isappend = (cmdrw_mop->slba == DEVFS_INVALID_SLBA) ? 1 : 0;

        /* read data block internally */
        retval = vfio_crfss_io_read(rd, cmdrw_mop, isappend);

micro_op_read_err:
        return retval;
}

long vfio_process_micro_op_write(struct crfss_fstruct *rd, nvme_cmdrw_t *cmdrw,
                                 nvme_cmdrw_t *cmdrw_mop, int vec_idx,
                                 char *io_buf, int buffer_write, int isappend) {
        long retval = 0;
        int i = vec_idx;
        char *p = NULL;
        char *prev = NULL;
        char *buf = NULL;
        int flush_io_buf = 0;

        if (cmdrw->param_vec[i].data_param.slba == MACRO_VEC_PREV) {
                /*
                 * This case is that the previous op is a computation
                 * i.e. compress. The compressed buffer already
                 * stored in cmdrw->common.prp_vec[i-1], with nlb in
                 * cmdrw->param_vec[i-1].data_param.nlb;
                 */
                cmdrw->param_vec[i].data_param.slba =
                    cmdrw->param_vec[i - 1].data_param.slba;
        }

        if (cmdrw->param_vec[i].data_param.nlb == MACRO_VEC_PREV) {
                /*
                 * This case is that the previous op is a computation
                 * i.e. compress. The compressed buffer already
                 * stored in cmdrw->common.prp_vec[i-1], with nlb in
                 * cmdrw->param_vec[i-1].data_param.nlb;
                 */
                cmdrw->param_vec[i].data_param.nlb =
                    cmdrw->param_vec[i - 1].data_param.nlb;
        }

        if (cmdrw->common.prp_vec[i] == MACRO_VEC_NA) {
                if (io_buf == NULL) {
                        /* for write macro op, data must be provided or come
                         * from previous op */
                        printk(KERN_ALERT
                               "DEBUG: no data provided for write macro op "
                               "%s:%d \n",
                               __FUNCTION__, __LINE__);
                        retval = -EFAULT;
                        goto micro_op_write_err;
                } else {
                        /* flush the internal io_buf */
                        buf = io_buf;
                        flush_io_buf = 1;
                }
        } else if (cmdrw->common.prp_vec[i] == MACRO_VEC_PREV) {
                if (i < 1 || cmdrw->common.prp_vec[i - 1] < 0) {
                        printk(KERN_ALERT "DEBUG: index error %s:%d \n",
                               __FUNCTION__, __LINE__);
                        retval = -EFAULT;
                        goto micro_op_write_err;
                }
                cmdrw->common.prp_vec[i] = cmdrw->common.prp_vec[i - 1];
                prev = (char *)cmdrw->common.prp_vec[i];
                if (buffer_write) {
                        memcpy(io_buf + cmdrw->param_vec[i].data_param.slba,
                               prev, cmdrw->param_vec[i].data_param.nlb);
                        kfree(prev);
                } else {
                        buf = prev;
                }
        } else {
                /*
                 * cmdrw->common.prp_vec[i] is provided, then override buf.
                 * Buffer may contains the data that read by previous op.
                 */
                if (buffer_write) {
                        buf = io_buf;
                } else {
                        buf = kmalloc(cmdrw->param_vec[i].data_param.nlb,
                                      GFP_KERNEL);
                        if (!buf) {
                                printk(KERN_ALERT
                                       "DEBUG: malloc failed %s:%d \n",
                                       __FUNCTION__, __LINE__);
                                retval = -EFAULT;
                                goto micro_op_write_err;
                        }
                }
                if (copy_from_user(buf + cmdrw->param_vec[i].data_param.slba,
                                   (void __user *)cmdrw->common.prp_vec[i],
                                   cmdrw->param_vec[i].data_param.nlb)) {
                        printk(KERN_ALERT "DEBUG: Failed %s:%d \n",
                               __FUNCTION__, __LINE__);
                        retval = -EFAULT;
                        goto micro_op_write_err;
                }
        }

        /*
         * Flush data if this operation is not a buffer write.
         */
        if (!buffer_write) {
                p = (__force char __user *)buf;
                cmdrw_mop->common.prp2 = (u64)p;
                cmdrw_mop->blk_addr = (u64)buf;
                cmdrw_mop->slba = cmdrw->param_vec[i].data_param.slba;
                cmdrw_mop->nlb = cmdrw->param_vec[i].data_param.nlb;
                if (isappend) {
                        cmdrw_mop->common.opc = nvme_cmd_append;
                        retval = vfio_crfss_io_append(rd, cmdrw_mop);
                } else {
                        cmdrw_mop->common.opc = nvme_cmd_write;
                        retval = vfio_crfss_io_write(rd, cmdrw_mop);
                }
#ifndef LEVELDB_OFFLOAD_CHECKSUM
                if (buf && !flush_io_buf) {
                        kfree(buf);
                }
#endif
        }
micro_op_write_err:
        return retval;
}

long vfio_process_micro_op_chksm(struct crfss_fstruct *rd, nvme_cmdrw_t *cmdrw,
                                 nvme_cmdrw_t *cmdrw_mop, int vec_idx,
                                 int num_op, char *buf) {
        long retval = 0;
        int i = vec_idx;
        char *p = NULL;
        uint32_t *crc = NULL;
        u64 slba = 0;

        if (cmdrw->common.prp_vec[i] == MACRO_VEC_NA) {
                /*
                 * This case is that the previous op is a data operation
                 * i.e. read. The data buffer already has data.
                 */
                if (buf == NULL) {
                        printk(KERN_ALERT "Invalid Buffer address %s:%d \n",
                               __FUNCTION__, __LINE__);
                        retval = -EFAULT;
                        goto micro_op_chksm_err;
                }

        } else if (cmdrw->common.prp_vec[i] == MACRO_VEC_PREV) {
                /*
                 * This case is that the previous op is a computation op.
                 * The data has been saved in cmdrw->common->prp_vec[i-1]
                 */
                if (i < 1 || cmdrw->common.prp_vec[i - 1] < 0) {
                        printk(KERN_ALERT "DEBUG: index error %s:%d \n",
                               __FUNCTION__, __LINE__);
                        retval = -EFAULT;
                        goto micro_op_chksm_err;
                }
                buf = (char *) cmdrw->common.prp_vec[i - 1];
        } else {
                /* The data comes from the user */
                if (copy_from_user(buf, (void __user *)cmdrw->common.prp_vec[i],
                                   cmdrw->param_vec[i].data_param.nlb)) {
                        printk(KERN_ALERT "DEBUG: Failed %s:%d \n",
                               __FUNCTION__, __LINE__);
                        retval = -EFAULT;
                        goto micro_op_chksm_err;
                }
                p = (__force char __user *)buf;
        }
        slba = cmdrw->param_vec[i].data_param.slba;
        /* calculate checksum and put in prp */
        crc = kmalloc(sizeof(uint32_t), GFP_KERNEL);
        *crc = crc32(0, buf + slba, cmdrw->param_vec[i].data_param.nlb);

        /* update nlb to be the size of crc value */
        cmdrw->common.prp_vec[i] = (u64)crc;
        cmdrw->param_vec[i].data_param.nlb = sizeof(uint32_t);

#ifdef _MACROFS_JOURN
        printk(KERN_ALERT "chm written = %x", crc);

        /* We only need to add log entries if the next vector is a write/append
         * op */
        if ((i + 1 < num_op) &&
            (cmdrw->common.opc_vec[i + 1] == nvme_cmd_append ||
             cmdrw->common.opc_vec[i + 1] == nvme_cmd_write)) {
                printk(KERN_ALERT "chm written = %x", crc);

                struct address_space *mapping = rd->fp->f_mapping;
                struct inode *inode = mapping->host;
                struct super_block *sb = inode->i_sb;
                struct crfs_inode *pi;

                void *xip_mem =
                    macrofs_get_xmem(rd->fp, sizeof(__u32), inode->i_size);

                /* add checksum log entry */
                crfs_macro_add_log_entry_data(rd, xip_mem, sizeof(__u32), &crc,
                                              LE_DATA, vec_idx);

                if (cmdrw->common.opc_vec[i + 1] == nvme_cmd_append) {
                        /*
                         * add inode log entry only if the next vector is an
                         * append operation because we need to add inode->i_size
                         */
                        pi = crfs_get_inode(sb, inode->i_ino);

                        loff_t new_isize = inode->i_size + sizeof(__u32);
                        crfs_macro_add_log_entry_data(rd, &(pi->i_size),
                                                      sizeof(loff_t),
                                                      &new_isize, LE_DATA, -1);
                }
        }

#endif
micro_op_chksm_err:
        return retval;
}

long vfio_process_micro_op_log_chksm(struct crfss_fstruct *rd,
                                     nvme_cmdrw_t *cmdrw,
                                     nvme_cmdrw_t *cmdrw_mop, int vec_idx,
                                     char *buf) {
        long retval = 0;
        int i = vec_idx;
        char *p = NULL;
        uint32_t *crc = NULL;
        u64 slba = 0;
        u64 nlb = 0;
        uint32_t a = 0;
        uint32_t b = 0;
        uint32_t length = 0;

        if (cmdrw->common.prp_vec[i] == MACRO_VEC_NA) {
                /*
                 * This case is that the previous op is a data operation
                 * i.e. read. The data buffer already has data.
                 */
                if (buf == NULL) {
                        printk(KERN_ALERT "Invalid Buffer address %s:%d \n",
                               __FUNCTION__, __LINE__);
                        retval = -EFAULT;
                        goto micro_op_chksm_err;
                }

        } else if (cmdrw->common.prp_vec[i] == MACRO_VEC_PREV) {
                /*
                 * This case is that the previous op is a computation op. The
                 * data has been saved in cmdrw->common->prp_vec[i-1]
                 *
                 */
                if (i < 1 || cmdrw->common.prp_vec[i - 1] < 0) {
                        printk(KERN_ALERT "DEBUG: index error %s:%d \n",
                               __FUNCTION__, __LINE__);
                        retval = -EFAULT;
                        goto micro_op_chksm_err;
                }
                buf = (char *)cmdrw->common.prp_vec[i - 1];
        } else {
                /* The data comes from the user */
                if (copy_from_user(buf, (void __user *)cmdrw->common.prp_vec[i],
                                   cmdrw->param_vec[i].data_param.nlb)) {
                        printk(KERN_ALERT "DEBUG: Failed %s:%d \n",
                               __FUNCTION__, __LINE__);
                        retval = -EFAULT;
                        goto micro_op_chksm_err;
                }
                p = (__force char __user *)buf;
        }

        a = (uint32_t)(((char *)buf)[4]) & 0xff;
        b = (uint32_t)(((char *)buf)[5]) & 0xff;
        length = a | (b << 8);

        nlb = length;

        slba = cmdrw->param_vec[i].data_param.slba;
        /* calculate checksum and put in prp */
        crc = kmalloc(sizeof(uint32_t), GFP_KERNEL);
        *crc = crc32(0, buf + slba, nlb);

        /* update nlb to be the size of crc value */
        cmdrw->common.prp_vec[i] = (u64)crc;
        //cmdrw->param_vec[i].data_param.nlb = sizeof(uint32_t);
        cmdrw->param_vec[i].data_param.nlb = length;

        // kfree(buf);

micro_op_chksm_err:
        return retval;
}

long vfio_process_micro_op_match(struct crfss_fstruct *rd, nvme_cmdrw_t *cmdrw,
                                 nvme_cmdrw_t *cmdrw_mop, int vec_idx,
                                 char *buf) {
        long retval = 0;
        int i = vec_idx;
        char *compare = NULL;
        u64 slba = 0;

        /* Value compare from previous op (Need to check pointer range first) */
        if (cmdrw->common.prp_vec[i] != MACRO_VEC_PREV) {
                /* Buffer addr not provide means something wrong happened */
                printk(KERN_ALERT "Invalid Buffer address %s:%d \n",
                       __FUNCTION__, __LINE__);
                retval = -EFAULT;
                goto micro_op_match_out;
        }
        compare = (char *)cmdrw->common.prp_vec[i - 1];

#if 0
        /* The previous io vec must be a data op instead of a cond op */
        if (cmdrw->param_vec[i].cond_param.nlb != MACRO_VEC_PREV) {
                /* Buffer addr not provide means something wrong happened */
                printk(KERN_ALERT "Invalid nlb %s:%d \n", __FUNCTION__,
                       __LINE__);
                retval = -EFAULT;
                goto micro_op_match_out;
        }
        cmdrw->param_vec[i].cond_param.nlb =
            cmdrw->param_vec[i - 1].data_param.nlb;
#endif
        slba = cmdrw->param_vec[i].cond_param.addr;
        /* Now compare the values */
	//printk(KERN_ALERT "nlb = %d\n", cmdrw->param_vec[i].cond_param.nlb);
        if (memcmp((const void *)(buf + slba), (const void *)compare,
                   cmdrw->param_vec[i].cond_param.nlb)) {
                printk(KERN_ALERT
                       "checksum mismatch, slba: %lld, actual = %x, read = %x\n",
                       slba, *(uint32_t *)compare, *(uint32_t *)(buf + slba));
                retval = -EFAULT;
        }
        if (compare) {
                kfree(compare);
        }
	cmdrw->param_vec[i].cond_param.nlb = cmdrw->param_vec[i-1].data_param.nlb;
micro_op_match_out:
        return retval;
}

long vfio_process_micro_op_compress(struct crfss_fstruct *rd,
                                    nvme_cmdrw_t *cmdrw, int vec_idx,
                                    char *inbuf) {
        long retval = 0;
        char *outbuf = NULL;
        size_t isize = 0;
        int i = vec_idx;
        size_t compress_len;
        struct dev_thread_struct *target_thread = NULL;

        target_thread = rd->dev_thread_ctx;
        if (!target_thread) {
                printk(KERN_ALERT "DEBUG: no target_thread! \n");
        }

        /*
         * if snappy compression environment variable is not set,
         * then initialize it.
         */
        if (!target_thread->g_snappy_init) {
                if (snappy_init_env(&target_thread->g_snappy_env)) {
                        printk(KERN_ALERT "DEBUG: Failed %s:%d \n",
                               __FUNCTION__, __LINE__);
                        goto micro_op_compress_out;
                }
                target_thread->g_snappy_init = 1;
                printk(KERN_ALERT "DEBUG: Snappy init successfully \n");
        }

        if (!inbuf) {
                printk(KERN_ALERT "%s, %d no input buffer for compression\n",
                       __FUNCTION__, __LINE__);
                goto micro_op_compress_out;
        }

        isize = cmdrw->param_vec[i].data_param.nlb;
        /*
         * Allocate kernel buffer to store the compressed data,
         * compressed file might have larger size than origin
         * file
         */
        outbuf = kmalloc(isize * 2, GFP_KERNEL);
        if (!outbuf) {
                printk(KERN_ALERT "%s, %d vmalloc fail\n", __FUNCTION__,
                       __LINE__);
                goto micro_op_compress_out;
        }
        memset(outbuf, 0, isize * 2);

        /*
         * printk(KERN_ALERT "start compress, input_size: %lu \n",
         *         isize);
         */

        if ((retval = snappy_compress(&target_thread->g_snappy_env,
                                      (const char *)inbuf, isize, outbuf,
                                      &compress_len)) != 0) {
                printk(KERN_ALERT "%s, %d compress failed\n", __FUNCTION__,
                       __LINE__);

                goto micro_op_compress_out;
        }

        cmdrw->common.prp_vec[i] =(u64) outbuf;
        cmdrw->param_vec[i].data_param.nlb = compress_len;
        return retval;

micro_op_compress_out:
        retval = -EFAULT;
        return retval;
}

typedef enum {
        DECRYPT = 0,
        ENCRYPT,
} crypto_direction_t;
#define AES_KEY_SIZE 16

long vfio_process_crypto(struct crfss_fstruct *rd, nvme_cmdrw_t *cmdrw,
                         nvme_cmdrw_t *cmdrw_mop, int vec_idx, char *buf,
                         crypto_direction_t rw) {
        long retval = 0;
        int i = vec_idx;
        int count = cmdrw->param_vec[i].data_param.nlb;

        u8 key[AES_KEY_SIZE];
        u8 iv[AES_KEY_SIZE] = "kjihgfedcba";
        struct crypto_skcipher *tfm = NULL;
        struct skcipher_request *req = NULL;
        struct scatterlist sg;

        tfm = crypto_alloc_skcipher("cbc(aes)", 0, 0);
        if (IS_ERR(tfm)) {
                printk(KERN_ALERT "%s: crypto_alloc_cipher() failed %p\n",
                       __func__, (void *)tfm);
                return -ENOMEM;
        }

        req = skcipher_request_alloc(tfm, GFP_KERNEL);
        if (!req) {
                printk(KERN_ALERT "req alloc failed\n");
                return -1;
        }

        memset(key, 0x61, AES_KEY_SIZE);
        if (crypto_skcipher_setkey(tfm, key, AES_KEY_SIZE)) {
                printk(KERN_ALERT "%s: crypto_cipher_setkey() failed %p\n",
                       __func__, (void *)tfm);
                return -EFAULT;
        }

        sg_init_one(&sg, buf, count);
        skcipher_request_set_crypt(req, &sg, &sg, count, iv);

        if (rw == ENCRYPT) {
                crypto_skcipher_encrypt(req);
        } else {
                crypto_skcipher_decrypt(req);
        }

        skcipher_request_free(req);
        crypto_free_skcipher(tfm);

        cmdrw->common.prp_vec[i] = (u64)buf;
        // cmdrw->param_vec[i].data_param.nlb = count;

micro_op_crypto_out:
        return retval;
}

long vfio_process_micro_op_close(struct crfss_fstruct *rd,
                                 nvme_cmdrw_t *cmdrw) {
        long retval = 0;
        if (rd != NULL) {
                if ((retval = crfss_close_inkernel(rd->fp, rd->fd))) {
                        printk(KERN_ALERT "%s:%d devfs_close failed\n",
                               __FUNCTION__, __LINE__);
                        goto micro_op_close_out;
                }
        }
        return retval;
micro_op_close_out:
        retval = -EFAULT;
        return retval;
}

long vfio_process_micro_op_slice(struct crfss_fstruct *rd, nvme_cmdrw_t *cmdrw, int vec_idx, char *buf)
{
	long retval = 0;
	int i = vec_idx;
	char *p = NULL;

	/* Buffer is from previous op */
	if (cmdrw->common.prp_vec[i] == MACRO_VEC_PREV) {
		cmdrw->common.prp_vec[i] = cmdrw->common.prp_vec[i-1] + 
						cmdrw->param_vec[i].cond_param.addr;
	} else if (cmdrw->common.prp_vec[i] == MACRO_VEC_NA) {
		cmdrw->common.prp_vec[i] = buf + 
						cmdrw->param_vec[i].cond_param.addr;
	}
	cmdrw->param_vec[i].cond_param.nlb = cmdrw->param_vec[i-1].cond_param.nlb;

	//rd->fp->f_pos = rd->fp->f_pos - 32768 + cmdrw->param_vec[i].cond_param.nlb + 7;
	rd->fp->f_pos = rd->fp->f_pos - cmdrw->common.ret[0] + cmdrw->param_vec[i].cond_param.nlb + 7;
	/*printk(KERN_ALERT "final slice buf = %llx, record_len = %d, fpos = %lu\n",
		cmdrw->common.prp_vec[i], cmdrw->param_vec[i].cond_param.nlb, rd->fp->f_pos);*/

micro_op_slice_out:
	return retval;
}

struct crfss_fstruct *vfio_process_micro_op_switch_file(nvme_cmdrw_t *cmdrw,int vec_idx)
{
	int i = vec_idx;
	int newfd;
	struct crfss_fstruct *newrd = NULL;
	u64 prev_nlb, prev_slba, prev_prp;

	if (i > 0) {
		prev_nlb = cmdrw->param_vec[i-1].data_param.nlb;
		prev_slba = cmdrw->param_vec[i-1].data_param.slba;
		prev_prp = cmdrw->common.prp_vec[i-1];
	}

	newfd = cmdrw->param_vec[i].data_param.nlb;
	newrd = fd_to_queuebuf(newfd);
	if (!newrd) {
		printk(KERN_ALERT "%s:%d fd_to_queuebuf failed %d\n",
			__FUNCTION__, __LINE__, newfd);
		goto micro_op_switch_file_out;
	}

	//printk(KERN_ALERT "Switching fd!\n");

	cmdrw->param_vec[i].data_param.nlb = prev_nlb;
	cmdrw->param_vec[i].data_param.slba = prev_slba;
	cmdrw->common.prp_vec[i] = prev_prp;

micro_op_switch_file_out:
	return newrd;
}

long vfio_process_micro_op_write_llist(struct crfss_fstruct *rd, nvme_cmdrw_t *cmdrw, int vec_idx, char *buf, int *dump)
{
	long retval = 0;
	int i = vec_idx;
	int sz = 0;
	void *kv = NULL;
	struct kvnode *node = NULL;

	/* Buffer is from previous op */
	if (cmdrw->common.prp_vec[i] == MACRO_VEC_PREV) {
		cmdrw->common.prp_vec[i] = cmdrw->common.prp_vec[i-1];
	} else if (cmdrw->common.prp_vec[i] == MACRO_VEC_NA) {
		cmdrw->common.prp_vec[i] = buf;
	}
	sz = cmdrw->param_vec[i-1].cond_param.nlb;

	/* Allocate kv buf */
	kv = kmalloc(sz, GFP_KERNEL);
	if (!kv) {
		printk(KERN_ALERT "DEBUG: kmalloc failed %s:%d \n",__FUNCTION__,__LINE__);
		retval = -ENOMEM;
		goto micro_op_write_llist_out;
	}
	memcpy(kv, (void*)cmdrw->common.prp_vec[i], sz);

	/* Allocate llist node */
	node = kmalloc(sizeof(struct kvnode), GFP_KERNEL);
	if (!node) {
		printk(KERN_ALERT "DEBUG: kmalloc failed %s:%d \n",__FUNCTION__,__LINE__);
		retval = -ENOMEM;
		goto micro_op_write_llist_out;
	}
 	node->key = kv;
	node->value = kv + 16;
	node->keysize = 16;
	node->valuesize = sz - 16;

	llist_size += sz;
	//printk(KERN_ALERT "llist_size = %d\n", llist_size);

	/* Add node to llist */	
	list_add(&node->list, &llist);

	*dump = (llist_size > _4M) ? 1 : 0;

	cmdrw->param_vec[i].cond_param.nlb = sz;
	retval = sz;

micro_op_write_llist_out:
	return retval;
}

int key_cmp(void *priv, struct list_head *a, struct list_head *b) {
	struct kvnode *nodea = container_of(a, struct kvnode, list);
	struct kvnode *nodeb = container_of(b, struct kvnode, list);

	return (strncmp(nodea->key, nodeb->key, 16) > 0);
}

long vfio_process_micro_op_sort_llist(struct crfss_fstruct *rd, nvme_cmdrw_t *cmdrw, int vec_idx, char *buf)
{
	long retval = 0;
	int i = vec_idx;
	void *final_buf = NULL;
	void *cur_loc = NULL;
	struct kvnode *node = NULL;

	/* Sort linked list */
	list_sort(NULL, &llist, key_cmp);

	/* Allocate final buffer */
	final_buf = vmalloc(llist_size);
	if (!final_buf) {
		printk(KERN_ALERT "DEBUG: kmalloc failed %s:%d \n",__FUNCTION__,__LINE__);
		retval = -ENOMEM;
		goto micro_op_sort_llist_out;
	}
	cur_loc = final_buf;

	/* Dump sorted kv to final buffer */
	list_for_each_entry(node, &llist, list) {
		int record_sz = node->keysize + node->valuesize;
		memcpy(cur_loc, node->key, record_sz);
		cur_loc += record_sz;
	}

	llist.next = &llist;
	llist.prev = &llist;

	cmdrw->param_vec[i].data_param.nlb = llist_size;
	cmdrw->param_vec[i].data_param.slba = DEVFS_INVALID_SLBA;
	cmdrw->common.prp_vec[i] = (u64)final_buf;

	llist_size = 0;

micro_op_sort_llist_out:
	return retval;
}






/*
 * Macro I/O parser for compound operations
 */
long vfio_devfs_macro_io_parser(struct crfss_fstruct *rd, nvme_cmdrw_t *cmdrw) {
        long ret = 0, retval = 0;
        int i = 0, num_op = 0;
        uint8_t vec_opcode = 0;
        char *buf = NULL;
        char *p = NULL;
	int dump = 0;
        nvme_cmdrw_t *cmdrw_mop = NULL;
        mm_segment_t oldfs = get_fs();

        /* Allocate cmdrw struct for each micro op */
        cmdrw_mop = kmalloc(sizeof(nvme_cmdrw_t), GFP_KERNEL);
        if (!cmdrw_mop) {
                printk(KERN_ALERT "DEBUG: kmalloc failed %s:%d \n",
                       __FUNCTION__, __LINE__);
                retval = -EFAULT;
                goto macro_io_parser_exit;
        }

        /* Allocate in-kernel buffer */
        buf = kmalloc(cmdrw->nlb, GFP_KERNEL);
        if (!buf) {
                printk(KERN_ALERT "DEBUG: malloc failed %s:%d \n", __FUNCTION__,
                       __LINE__);
                retval = -EFAULT;
                goto macro_io_parser_exit;
        }
        p = (__force char __user *)buf;
        cmdrw_mop->common.prp2 = (uint64_t)p;
        memcpy(cmdrw_mop->cred_id, cmdrw->cred_id, CRED_ID_BYTES);
        /* Set free segment to fake a kernel pointer as user pointer */
        set_fs(KERNEL_DS);
        /* Get num ops per compound I/O */
        num_op = cmdrw->common.num_op;

#ifdef _MACROFS_JOURN
        /* Strat transaction init */
        struct inode *inode = rd->fp->f_inode;
        struct super_block *sb = inode->i_sb;
        struct crfs_inode *pi = crfs_get_inode(sb, inode->i_ino);

        /* Initialize a new transaction with given micro ops */
        crfs_macro_transaction_init(rd, cmdrw, num_op);

        /* Add meta-data log entry */
        crfs_macro_add_log_entry(rd, pi, MAX_DATA_PER_LENTRY, LE_DATA, -1);
	
	/* Add op log */
	char fname[256];
	char *log = kmalloc(4096, GFP_KERNEL);
	int log_pos = 0;
	int log_data_pos = 0;

	/* Mark the start of the this op log */
	log[log_pos++] = '|';

	/* Add target file info to op log file */
	dentry_path_raw(rd->fp->f_path.dentry, fname, 256);
	memcpy(log + log_pos, fname, strlen(fname));
	log_pos += strlen(fname);
	log[log_pos++] = '#';

	/* Copy the cmd as op log into op log file */
	memcpy(log + log_pos, cmdrw, sizeof(nvme_cmdrw_t));
	log_pos += sizeof(nvme_cmdrw_t);

	/* Append op log to the log file */
	 crfs_xip_file_write(op_log, (__force const char __user *)log, log_pos, &(op_log->f_pos));

	kfree(log);
#endif

        /* Iterate and process each op */
        for (i = 0; i < num_op; ++i) {
                vec_opcode = cmdrw->common.opc_vec[i];

                switch (vec_opcode) {
                        case nvme_cmd_open:
                                rd = vfio_process_micro_op_open(cmdrw, i);
                                if (rd == NULL) {
                                        cmdrw->common.ret[i] = -EFAULT;
                                        goto macro_io_parser_exit;
                                }
                                break;
                        case nvme_cmd_read:
                                ret = vfio_process_micro_op_read(
                                    rd, cmdrw, cmdrw_mop, i, buf);
                                retval += ret;
#ifdef LEVELDB_OFFLOAD_CHECKSUM
				if (ret == 0)
                                        goto macro_io_parser_exit;
#endif
                                break;
                        case nvme_cmd_write_buffer:
                                ret = vfio_process_micro_op_write(
                                    rd, cmdrw, cmdrw_mop, i, buf, 1, 0);
                                break;
                        case nvme_cmd_write:
                                ret = vfio_process_micro_op_write(
                                    rd, cmdrw, cmdrw_mop, i, buf, 0, 0);
                                retval = ret;
                                break;
                        case nvme_cmd_append:
                                ret = vfio_process_micro_op_write(
                                    rd, cmdrw, cmdrw_mop, i, buf, 0, 1);
                                retval = ret;
                                break;
                        case nvme_cmd_close:
                                ret = vfio_process_micro_op_close(rd, cmdrw);
                                break;
                        case nvme_cmd_chksm:
                                ret = vfio_process_micro_op_chksm(
                                    rd, cmdrw, cmdrw_mop, i, num_op, buf);
                                break;
                        case nvme_cmd_leveldb_log_chksm:
#ifdef LEVELDB_OFFLOAD_CHECKSUM
				if (cmdrw->common.ret[0] < 7) {
					printk(KERN_ALERT "Bad record size!\n");
					retval = 0;	
					goto macro_io_parser_exit;
				}
#endif
                                ret = vfio_process_micro_op_log_chksm(
                                    rd, cmdrw, cmdrw_mop, i, buf);
                                break;
                        case nvme_cmd_compress:
                                ret = vfio_process_micro_op_compress(rd, cmdrw,
                                                                     i, buf);
                                break;
                        case nvme_cmd_decompress:
                                // TODO
                                // retval = vfio_process_micro_op_uncompress(rd,
                                // cmdrw, cmdrw_mop, i);
                                break;
                        case nvme_cmd_match:
                                ret = vfio_process_micro_op_match(
                                    rd, cmdrw, cmdrw_mop, i, buf);
                                break;
                        case nvme_cmd_encrypt:
                                ret = vfio_process_crypto(rd, cmdrw, cmdrw_mop,
                                                          i, buf, ENCRYPT);
                                break;
                        case nvme_cmd_decrypt:
                                ret = vfio_process_crypto(rd, cmdrw, cmdrw_mop,
                                                          i, buf, DECRYPT);
                                break;
			case nvme_cmd_slice:
				ret = vfio_process_micro_op_slice(rd, cmdrw, i, buf);
				break;
			case nvme_cmd_switch_file:
				rd = vfio_process_micro_op_switch_file(cmdrw, i);
				if (rd == NULL) {
					cmdrw->common.ret[i] = -EFAULT;
					goto macro_io_parser_exit;
				}
				break;
			case nvme_cmd_write_llist:
				ret = vfio_process_micro_op_write_llist(rd, cmdrw, i, buf, &dump);
				if (dump == 0) {
					retval = ret;
					goto macro_io_parser_exit;
				}
				break;
			case nvme_cmd_sort_llist:
				ret = vfio_process_micro_op_sort_llist(rd, cmdrw, i, buf);
				break;
                        default:
                                printk(KERN_ALERT "unrecognized micro op!\n");
                                retval = -EFAULT;
                                break;
                }
                cmdrw->common.ret[i] = ret;
                if (ret < 0) {
                        retval = -EFAULT;
                        goto macro_io_parser_exit;
                }

#ifdef _MACROFS_JOURN
                /*
                 * After a micro op finishes set commit bit in-place
                 * in the journal entry for this micro op in the loop
                 */
                crfs_macro_commit_micro_trans(rd, i);
#endif
		/* 
		 * Injected crash for append-checksum-write
		 * More details please see Github README
		 */
                if (crash_after_checksum == 1 && i == 2) {
                        printk(KERN_ALERT "injected crash happened!\n");
                        retval = -EFAULT;
                        goto macro_io_parser_exit;
                }
        }

        if (cmdrw->common.prp2 != MACRO_VEC_NA) {
                /*
                 * This case means there is buffer passed from user space,
                 * so this is read operation. We need to copy the data back.
                 */
                if (copy_to_user((void __user *)cmdrw->common.prp2, buf,
                                 retval)) {
                        printk(KERN_ALERT "DEBUG: Failed %s:%d \n",
                               __FUNCTION__, __LINE__);
                        retval = -EFAULT;
                        goto macro_io_parser_exit;
                }
        }

macro_io_parser_exit:
        if (buf) kfree(buf);

        if (cmdrw_mop) kfree(cmdrw_mop);

        /* Set fs register back */
        set_fs(oldfs);
        return retval;
}
