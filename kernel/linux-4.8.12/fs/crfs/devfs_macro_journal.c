/* 
 * devfs_macro_journal.c
 *
 * Description: Macro I/O Journaling Mechanisms
 *
 */
#include <linux/fs.h>
#include <linux/devfs.h>
#include <linux/file.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/nvme.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/pagemap.h>
#include <linux/crc32.h>
#include <linux/snappy.h>
#include <linux/vfio.h>
#include "pmfs.h"
#include "xip.h"
#include "journal.h"

/* Micro-transaction journaling mechanism */
int crfs_macro_transaction_init(struct crfss_fstruct *rd, nvme_cmdrw_t *cmdrw,
				int num_micro_op) {
	int retval = 0, i = 0;
	crfs_transaction_t *trans = NULL;
	struct inode *inode = rd->fp->f_inode;
	struct super_block *sb = inode->i_sb;
	struct crfs_inode *pi = NULL;
	size_t count, offset;
	loff_t pos;
	unsigned long num_blocks, max_logentries;
	bitmap_t bitmap = NULL;
	char macro_log_data[MAX_DATA_PER_LENTRY];

	pos = (loff_t)cmdrw->slba;
	count = (size_t)cmdrw->nlb;
	pi = crfs_get_inode(sb, inode->i_ino);

	offset = pos & (sb->s_blocksize - 1);
	num_blocks = ((count + offset - 1) >> sb->s_blocksize_bits) + 1;

	/* Allocate transaction */
	max_logentries = num_blocks / MAX_PTRS_PER_LENTRY + 2;
	if (max_logentries > MAX_METABLOCK_LENTRIES)
		max_logentries = MAX_METABLOCK_LENTRIES;

	trans = crfs_new_transaction(sb, MAX_INODE_LENTRIES + max_logentries);
	if (IS_ERR(trans)) {
		printk(KERN_ALERT "DEBUG: Failed %s:%d \n",__FUNCTION__,__LINE__);
		retval = -EFAULT;
		goto macro_transaction_init_exit;
	}

	/* Assign num of micro ops in a macro operation */
	trans->num_micro_op = num_micro_op;

	/* Initialize micro op bitmap and commit bitmap in transaction head */
	bitmap = (bitmap_t)trans->mop_bitmap;
	memset(bitmap, 0, MACRO_BITMAP_SIZE);
	for (i = 0; i < num_micro_op; ++i)
		set_bitmap(bitmap, i);

	bitmap = (bitmap_t)trans->mop_commit_bitmap;
	memset(bitmap, 0, MACRO_BITMAP_SIZE);

	/* Persist micro op bitmap and zero commit bitmap */
	memset(macro_log_data, 0, MAX_DATA_PER_LENTRY);
	memcpy(macro_log_data, trans->mop_bitmap, MACRO_BITMAP_SIZE);
	memcpy(macro_log_data + MACRO_BITMAP_SIZE, 
		trans->mop_commit_bitmap, MACRO_BITMAP_SIZE);
	crfs_add_logentry(sb, trans, macro_log_data, MAX_DATA_PER_LENTRY, LE_MACRO);

	/* Relate this new transaction to corresponding rd */
	rd->trans = (void*)trans;

macro_transaction_init_exit:
	return retval;
}

int crfs_macro_add_log_entry(struct crfss_fstruct *rd, void *addr,
			uint16_t size, u8 type, int16_t micro_op_idx) {
	int retval = 0;
	crfs_transaction_t *trans = rd->trans;
	struct inode *inode = rd->fp->f_inode;
	struct super_block *sb = inode->i_sb;
	crfs_logentry_t *le = NULL;
	uint16_t le_off = 0;
	//struct crfs_inode *pi = crfs_get_inode(sb, inode->i_ino);

	if (trans == NULL) {
		printk(KERN_ALERT "DEBUG: Failed %s:%d \n",__FUNCTION__,__LINE__);
		retval = -EFAULT;
		goto macro_add_log_entry_exit;
	}

	/* log entry offset for current micro op */	
	le_off = trans->num_used;

	/* Add log entry for a micro operation */
	retval = crfs_add_logentry(sb, trans, addr, size, type);
	if (retval) {
		printk(KERN_ALERT "DEBUG: Failed %s:%d \n",__FUNCTION__,__LINE__);
		goto macro_add_log_entry_exit;
	}

	/* Record the start log entry pointer for this micro-trans */
	if (micro_op_idx >= 0) {
		/* bitmap resides in the first log entry of transaction */
		le = trans->start_addr;
		if (!le) {
			printk(KERN_ALERT "DEBUG: Failed %s:%d \n",__FUNCTION__,__LINE__);
			retval = -EFAULT;
			goto macro_add_log_entry_exit;
		}
		memcpy(le->data + 2*MACRO_BITMAP_SIZE + micro_op_idx*sizeof(int16_t), 
			&le_off, sizeof(int16_t));
		crfs_flush_buffer(le->data, MAX_DATA_PER_LENTRY, false);
		PERSISTENT_MARK();
		PERSISTENT_BARRIER();
	}

macro_add_log_entry_exit:
	return retval;
}

int crfs_macro_add_log_entry_data(struct crfss_fstruct *rd, void *addr,
			uint16_t size, void *data, u8 type, int16_t micro_op_idx) {
	int retval = 0;
	crfs_transaction_t *trans = rd->trans;
	struct inode *inode = rd->fp->f_inode;
	struct super_block *sb = inode->i_sb;
	crfs_logentry_t *le = NULL;
	uint16_t le_off = 0;
	//struct crfs_inode *pi = crfs_get_inode(sb, inode->i_ino);

	if (trans == NULL) {
		printk(KERN_ALERT "DEBUG: Failed %s:%d \n",__FUNCTION__,__LINE__);
		retval = -EFAULT;
		goto macro_add_log_entry_exit;
	}

	/* log entry offset for current micro op */	
	le_off = trans->num_used;

	/* Add log entry for a micro operation */
	retval = crfs_add_logentry_data(sb, trans, addr, size, data, type);
	if (retval) {
		printk(KERN_ALERT "DEBUG: Failed %s:%d \n",__FUNCTION__,__LINE__);
		goto macro_add_log_entry_exit;
	}

	/* Record the start log entry pointer for this micro-trans */
	if (micro_op_idx >= 0) {
		/* bitmap resides in the first log entry of transaction */
		le = trans->start_addr;
		if (!le) {
			printk(KERN_ALERT "DEBUG: Failed %s:%d \n",__FUNCTION__,__LINE__);
			retval = -EFAULT;
			goto macro_add_log_entry_exit;
		}
		memcpy(le->data + 2*MACRO_BITMAP_SIZE + micro_op_idx*sizeof(int16_t), 
			&le_off, sizeof(int16_t));
		crfs_flush_buffer(le->data, MAX_DATA_PER_LENTRY, false);
		PERSISTENT_MARK();
		PERSISTENT_BARRIER();
	}

macro_add_log_entry_exit:
	return retval;
}


int crfs_macro_commit_micro_trans(struct crfss_fstruct *rd, int micro_op_idx) {
	int retval = 0;
	crfs_transaction_t *trans = rd->trans;
	struct inode *inode = rd->fp->f_inode;
	struct super_block *sb = inode->i_sb;
	crfs_logentry_t *le = NULL;
	bitmap_t bitmap = NULL;
	//struct crfs_inode *pi = crfs_get_inode(sb, inode->i_ino);

	bitmap = (bitmap_t)trans->mop_commit_bitmap;
	set_bitmap(bitmap, micro_op_idx);

	/* bitmap resides in the first log entry of transaction */
	le = trans->start_addr;
	if (!le) {
		printk(KERN_ALERT "DEBUG: Failed %s:%d \n",__FUNCTION__,__LINE__);
		retval = -EFAULT;
		goto macro_commit_micro_trans_exit;
	}
	memcpy(le->data + MACRO_BITMAP_SIZE, bitmap, MACRO_BITMAP_SIZE);
	crfs_flush_buffer(le->data, MAX_DATA_PER_LENTRY, false);
	PERSISTENT_MARK();
	PERSISTENT_BARRIER();

	/* flush the entire transaction if this is the last micro op */
	if (micro_op_idx == trans->num_micro_op - 1) {
		/* the final step: commit the whole transaction */
		crfs_commit_transaction(sb, trans);
		printk(KERN_ALERT "transaction has been flushed!\n");
	}

macro_commit_micro_trans_exit:
	return retval;
}

void crfs_macro_recover_trans(crfs_transaction_t *trans, crfs_logentry_t *le) {
	crfs_micro_op_data micro_trans = *(crfs_micro_op_data *)le->data;
	uint64_t mop_bitmap;
	uint64_t mop_commit_bitmap;

	if (le->type & LE_MACRO) {
		trans->is_micro_trans = true;
		mop_bitmap = le64_to_cpu(micro_trans.mop_bitmap);
		memcpy(trans->mop_bitmap, &mop_bitmap, sizeof(mop_bitmap));
		mop_commit_bitmap = le64_to_cpu(micro_trans.mop_commit_bitmap);
		memcpy(trans->mop_commit_bitmap, &mop_commit_bitmap, sizeof(mop_commit_bitmap));
		trans->num_micro_op = get_set_bits(&trans->mop_bitmap);

		//printk(KERN_ALERT "num_micro_op = %d\n", trans->num_micro_op);
		//printk(KERN_ALERT "mop_bitmap = %lu\n", mop_bitmap);
		//printk(KERN_ALERT "mop_commit_bitmap = %lu\n", mop_commit_bitmap);
	} else {
		trans->is_micro_trans = false;
	}
}

void crfs_macro_replay_transaction(struct super_block *sb,
		crfs_transaction_t *trans, bool recover) {
	int i = 0, micro_op_le_idx = 0;
	uint16_t gen_id = trans->gen_id;
	char *data;

	/* bitmap and pointers resides in the first log entry of transaction */
	crfs_logentry_t *meta_le = trans->start_addr;
	crfs_logentry_t *le = meta_le + 1;
	crfs_micro_op_data *micro_op_meta = (crfs_micro_op_data*)meta_le->data;

	/* log entry offset array for each micro op */
	uint16_t *mop_ptr = micro_op_meta->mop_ptr;

	/*printk(KERN_ALERT "$$$$ num used = %d", trans->num_used);
	for (i = 0; i < trans->num_micro_op; ++i)
		printk(KERN_ALERT "********* off = %d", mop_ptr[i]);*/

	for (micro_op_le_idx = 0; micro_op_le_idx < trans->num_micro_op; ++micro_op_le_idx) {
		int cur_le_off = micro_op_meta->mop_ptr[micro_op_le_idx];
		int next_le_off = trans->num_used;
		int off;
		if (cur_le_off == 0) {
			/* No log entries in this micro transaction */
			continue;
		}
		for (off = cur_le_off; off < next_le_off; ++off) {
			le = meta_le + off;

			if (gen_id == le16_to_cpu(le->gen_id) && le->size > 0) {
				data = crfs_get_block(sb,le64_to_cpu(le->addr_offset));
				//printk(KERN_ALERT "data = %lx", data);
				//printk(KERN_ALERT "le->data = %lx", *(uint32_t*)le->data);

				/* flush data if we are called during recovery */
				if (recover) {
					crfs_memunlock_range(sb, data, le->size);
					memcpy(data, le->data, le->size);
					crfs_memlock_range(sb, data, le->size);
				}
				crfs_flush_buffer(data, le->size, false);
			}

		}

	}

}

/* Micro-transaction recovery mechanism */
