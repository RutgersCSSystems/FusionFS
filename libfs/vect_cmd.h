#ifndef VECT_CMD_H
#define VECT_CMD_H

#define _GNU_SOURCE
#include "fusionfsio.h"
#include "unvme_nvme.h"
#include "vfio.h"

/*
 * LevelDB vector command
 */
struct macro_op_desc leveldb_checksumpread_cmd(void *buf, size_t count, int end,
                                               off_t offset);

struct macro_op_desc readchecksumappend_cmd(size_t count, int fd_1);

/*
 * Normal vector command
 */
struct macro_op_desc checksumread_cmd(void *buf, size_t count);

struct macro_op_desc checksumpread_cmd(void *buf, size_t count, off_t offset);

struct macro_op_desc checksumwrite_cmd(const void *buf, size_t count);

struct macro_op_desc checksumpwrite_cmd(const void *buf, size_t count,
                                        off_t offset);

struct macro_op_desc readmodifywrite_cmd(const void *buf, size_t count,
                                         off_t offset);

struct macro_op_desc readmodifyappend_cmd(const void *p, size_t count);

struct macro_op_desc checksumwritecc_cmd(const void *buf, size_t count);

struct macro_op_desc encryptwrite_cmd(const void *buf, size_t count);

struct macro_op_desc encryptpwrite_cmd(const void *buf, size_t count,
                                       off_t offset);

struct macro_op_desc preadencryptpwrite_cmd(const void *buf, size_t count,
                                            off_t offset);

#endif
