#ifndef _CRASH_CODE_
#define _CRASH_CODE_

/* Simple write/append */
#define CRASH_BEFORE_INODE_LOG  0x01
#define CRASH_IN_INODE_LOG      0x02
#define CRASH_AFTER_INODE_LOG   0x03
#define CRASH_BEFORE_BALLOC     0x04
#define CRASH_IN_BALLOC         0x05
#define CRASH_AFTER_BALLOC      0x06

/* Write-and-checksum */
#define CRASH_AFTER_CHECKSUM    0x07

#endif
