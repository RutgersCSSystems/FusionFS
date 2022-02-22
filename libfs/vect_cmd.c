#define _GNU_SOURCE

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <linux/pci.h>
#include <linux/types.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "unvme_nvme.h"
#include "vfio.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include "crc32/crc32_defs.h"
#include "fusionfsio.h"
#include "time_delay.h"
#include "vect_cmd.h"

struct macro_op_desc readmodifywrite_cmd(const void *p, size_t count,
                                         off_t offset) {
        struct macro_op_desc desc;
        desc.num_op = 2;
        desc.iov[0].opc = nvme_cmd_read;
        desc.iov[0].prp = MACRO_VEC_NA;
        desc.iov[0].slba = offset;
        desc.iov[0].nlb = count;
        desc.iov[1].opc = nvme_cmd_write;
        desc.iov[1].prp = (uint64_t)p;
        desc.iov[1].slba = 0;
        desc.iov[1].nlb = count;
        return desc;
}

struct macro_op_desc readmodifyappend_cmd(const void *p, size_t count) {
        struct macro_op_desc desc;
        desc.num_op = 2;
        desc.iov[0].opc = nvme_cmd_read;
        desc.iov[0].prp = MACRO_VEC_NA;
        desc.iov[0].slba = INVALID_SLBA;
        desc.iov[0].nlb = count;
        desc.iov[1].opc = nvme_cmd_append;
        desc.iov[1].prp = (uint64_t)p;
        desc.iov[1].slba = 0;
        desc.iov[1].nlb = count;
        return desc;
}

struct macro_op_desc checksumwrite_cmd(const void *p, size_t count) {
        struct macro_op_desc desc;
        desc.num_op = 4;
        desc.iov[0].opc = nvme_cmd_write_buffer;
        desc.iov[0].prp = (uint64_t)p;
        desc.iov[0].slba = 0;
        desc.iov[0].nlb = count - 4;
        desc.iov[1].opc = nvme_cmd_chksm;
        desc.iov[1].prp = MACRO_VEC_NA;
        desc.iov[1].slba = 0;
        desc.iov[1].nlb = count - 4;
        desc.iov[2].opc = nvme_cmd_write_buffer;
        desc.iov[2].prp = MACRO_VEC_PREV;
        desc.iov[2].slba = count - 4;
        desc.iov[2].nlb = MACRO_VEC_PREV;
        desc.iov[3].opc = nvme_cmd_append;
        desc.iov[3].prp = MACRO_VEC_NA;
        desc.iov[3].slba = INVALID_SLBA;
        desc.iov[3].nlb = count;

        return desc;
}

struct macro_op_desc checksumpwrite_cmd(const void *p, size_t count,
                                        off_t offset) {
        struct macro_op_desc desc;

        /* Build I/O vec */
        desc.num_op = 4;
        desc.iov[0].opc = nvme_cmd_write_buffer;
        desc.iov[0].prp = (uint64_t)p;
        desc.iov[0].slba = 0;
        desc.iov[0].nlb = count - 4;
        desc.iov[1].opc = nvme_cmd_chksm;
        desc.iov[1].prp = MACRO_VEC_NA;
        desc.iov[1].slba = 0;
        desc.iov[1].nlb = count - 4;
        desc.iov[2].opc = nvme_cmd_write_buffer;
        desc.iov[2].prp = MACRO_VEC_PREV;
        desc.iov[2].slba = count - 4;
        desc.iov[2].nlb = MACRO_VEC_PREV;
        desc.iov[3].opc = nvme_cmd_write;
        desc.iov[3].prp = MACRO_VEC_NA;
        desc.iov[3].slba = offset;
        desc.iov[3].nlb = count;

        return desc;
}

struct macro_op_desc checksumpread_cmd(void *p, size_t count, off_t offset) {
        struct macro_op_desc desc;
        /* Build I/O vec */
        desc.num_op = 3;
        desc.iov[0].opc = nvme_cmd_read;
        desc.iov[0].prp = (uint64_t)p;
        desc.iov[0].slba = offset;
        desc.iov[0].nlb = count;
        desc.iov[1].opc = nvme_cmd_chksm;
        desc.iov[1].prp = MACRO_VEC_NA;
        desc.iov[1].slba = 0;
        desc.iov[1].nlb = count - 4;
        desc.iov[2].opc = nvme_cmd_match;
        desc.iov[2].prp = MACRO_VEC_PREV;
        desc.iov[2].addr = count - 4;
        desc.iov[2].nlb = MACRO_VEC_PREV;

        return desc;
}

struct macro_op_desc checksumread_cmd(void *p, size_t count) {
        struct macro_op_desc desc;
        /* Build I/O vec */
        desc.num_op = 3;
        desc.iov[0].opc = nvme_cmd_read;
        desc.iov[0].prp = (uint64_t)p;
        desc.iov[0].slba = INVALID_SLBA;
        desc.iov[0].nlb = count;
        desc.iov[1].opc = nvme_cmd_chksm;
        desc.iov[1].prp = MACRO_VEC_NA;
        desc.iov[1].slba = 0;
        desc.iov[1].nlb = count - 4;
        desc.iov[2].opc = nvme_cmd_match;
        desc.iov[2].prp = MACRO_VEC_PREV;
        desc.iov[2].addr = count - 4;
        desc.iov[2].nlb = MACRO_VEC_PREV;

        return desc;
}

struct macro_op_desc leveldb_checksumpread_cmd(void *buf, size_t count, int end,
                                               off_t offset) {
        struct macro_op_desc desc;
        /* Build I/O vec */
        desc.num_op = 3;
        if (end) {
                desc.iov[0].opc = nvme_cmd_read;
                desc.iov[0].prp = (uint64_t)buf;
                desc.iov[0].slba = offset;
                desc.iov[0].nlb = count;
                desc.iov[1].opc = nvme_cmd_chksm;
                desc.iov[1].prp = MACRO_VEC_NA;
                desc.iov[1].slba = 0;
                desc.iov[1].nlb = count - 5;
                desc.iov[2].opc = nvme_cmd_match;
                desc.iov[2].prp = MACRO_VEC_PREV;
                desc.iov[2].addr = count - 4;
                //desc.iov[2].nlb = MACRO_VEC_PREV;
                desc.iov[2].nlb = 4;
        } else {
                desc.iov[0].opc = nvme_cmd_read;
                desc.iov[0].prp = (uint64_t)buf;
                desc.iov[0].slba = offset;
                desc.iov[0].nlb = count;
                desc.iov[1].opc = nvme_cmd_chksm;
                desc.iov[1].prp = MACRO_VEC_NA;
                desc.iov[1].slba = 7;
                desc.iov[1].nlb = count - 7;
                desc.iov[2].opc = nvme_cmd_match;
                desc.iov[2].prp = MACRO_VEC_PREV;
                desc.iov[2].addr = 0;
                //desc.iov[2].nlb = MACRO_VEC_PREV;
                desc.iov[2].nlb = 4;
        }
        return desc;
}

struct macro_op_desc checksumwritecc_cmd(const void *p, size_t count) {
        struct macro_op_desc desc;
        desc.num_op = 4;
        desc.iov[0].opc = nvme_cmd_write_buffer;
        desc.iov[0].prp = (uint64_t)p;
        desc.iov[0].slba = 0;
        desc.iov[0].nlb = count - 4;
        desc.iov[1].opc = nvme_cmd_append;
        desc.iov[1].prp = MACRO_VEC_NA;
        desc.iov[1].slba = INVALID_SLBA;
        desc.iov[1].nlb = count - 4;
        desc.iov[2].opc = nvme_cmd_chksm;
        desc.iov[2].prp = MACRO_VEC_NA;
        desc.iov[2].slba = 0;
        desc.iov[2].nlb = count - 4;
        desc.iov[3].opc = nvme_cmd_append;
        desc.iov[3].prp = MACRO_VEC_PREV;
        desc.iov[3].slba = MACRO_VEC_NA;
        desc.iov[3].nlb = 4;

        return desc;
}

struct macro_op_desc encryptwrite_cmd(const void *p, size_t count) {
        struct macro_op_desc desc;
        desc.num_op = 3;
        desc.iov[0].opc = nvme_cmd_write_buffer;
        desc.iov[0].prp = (uint64_t)p;
        desc.iov[0].slba = 0;
        desc.iov[0].nlb = count;
        desc.iov[1].opc = nvme_cmd_encrypt;
        desc.iov[1].prp = MACRO_VEC_NA;
        desc.iov[1].slba = 0;
        desc.iov[1].nlb = count;
        desc.iov[2].opc = nvme_cmd_append;
        desc.iov[2].prp = MACRO_VEC_NA;
        desc.iov[2].slba = MACRO_VEC_NA;
        desc.iov[2].nlb = MACRO_VEC_PREV;

        return desc;
}

struct macro_op_desc encryptpwrite_cmd(const void *p, size_t count,
                                       off_t offset) {
        struct macro_op_desc desc;
        desc.num_op = 3;
        desc.iov[0].opc = nvme_cmd_write_buffer;
        desc.iov[0].prp = (uint64_t)p;
        desc.iov[0].slba = 0;
        desc.iov[0].nlb = count;
        desc.iov[1].opc = nvme_cmd_encrypt;
        desc.iov[1].prp = MACRO_VEC_NA;
        desc.iov[1].slba = 0;
        desc.iov[1].nlb = count;
        desc.iov[2].opc = nvme_cmd_write;
        desc.iov[2].prp = MACRO_VEC_NA;
        desc.iov[2].slba = offset;
        desc.iov[2].nlb = MACRO_VEC_PREV;

        return desc;
}

struct macro_op_desc preadencryptpwrite_cmd(const void *buf, size_t count,
                                            off_t offset) {
        struct macro_op_desc desc;
        desc.num_op = 3;
        desc.iov[0].opc = nvme_cmd_read;
        desc.iov[0].prp = MACRO_VEC_NA;
        desc.iov[0].slba = offset;
        desc.iov[0].nlb = count;
        desc.iov[1].opc = nvme_cmd_encrypt;
        desc.iov[1].prp = MACRO_VEC_NA;
        desc.iov[1].slba = 0;
        desc.iov[1].nlb = count;
        desc.iov[2].opc = nvme_cmd_write;
        desc.iov[2].prp = MACRO_VEC_NA;
        desc.iov[2].slba = offset;
        desc.iov[2].nlb = MACRO_VEC_PREV;

        return desc;
}

struct macro_op_desc readchecksumappend_cmd(size_t count, int fd_1) {
	struct macro_op_desc desc;
	/* Build I/O vec */
	desc.num_op = 8;
	desc.iov[0].opc = nvme_cmd_read;
	desc.iov[0].prp = MACRO_VEC_NA;
	desc.iov[0].slba = INVALID_SLBA;
	desc.iov[0].nlb = count;
	desc.iov[1].opc = nvme_cmd_leveldb_log_chksm;
	desc.iov[1].prp = MACRO_VEC_NA;
	desc.iov[1].slba = 7;
	desc.iov[1].nlb = MACRO_VEC_PREV;
	desc.iov[2].opc = nvme_cmd_match;
	desc.iov[2].prp = MACRO_VEC_PREV;
	desc.iov[2].addr = 0;
	desc.iov[2].nlb = 4;
	desc.iov[3].opc = nvme_cmd_slice;
	desc.iov[3].prp = MACRO_VEC_NA;
	desc.iov[3].slba = 7;
	desc.iov[3].nlb = MACRO_VEC_PREV;
	desc.iov[4].opc = nvme_cmd_write_llist;
	desc.iov[4].prp = MACRO_VEC_PREV;
	desc.iov[4].slba = MACRO_VEC_NA;
	desc.iov[4].nlb = MACRO_VEC_PREV;
	desc.iov[5].opc = nvme_cmd_sort_llist;
	desc.iov[5].prp = MACRO_VEC_PREV;
	desc.iov[5].slba = MACRO_VEC_NA;
	desc.iov[5].nlb = MACRO_VEC_PREV;
	desc.iov[6].opc = nvme_cmd_switch_file;
	desc.iov[6].prp = MACRO_VEC_NA;
	desc.iov[6].slba = MACRO_VEC_NA;
	desc.iov[6].nlb = fd_1;
	desc.iov[7].opc = nvme_cmd_append;
	desc.iov[7].prp = MACRO_VEC_PREV;
	desc.iov[7].slba = MACRO_VEC_PREV;
	desc.iov[7].nlb = MACRO_VEC_PREV;

	return desc;
}
