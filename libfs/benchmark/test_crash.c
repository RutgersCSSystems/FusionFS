#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>

#include "../unvme_nvme.h"
#include "../fusionfslib.h"
#include "crash_code.h"

#define TESTDIR "/mnt/ram"

#define BLOCKSIZE 16384
#define FSPATHLEN 256
#define ITERS 100
#define FILEPERM 0666
#define DIRPERM 0755

char buf[BLOCKSIZE];

int main(int argc, char **argv) {
	int i, fd = 0, ret = 0;
	int crash_pos;

	if (argc < 2) {
		printf("Usage: ./crash_test <crash_pos>\n");
		printf("CRASH_BEFORE_INODE_LOG: 1\n");
		printf("CRASH_IN_INODE_LOG: 2\n");
		printf("CRASH_AFTER_INODE_LOG: 3\n");
		printf("CRASH_BEFORE_BALLOC: 4\n");
		printf("CRASH_IN_BALLOC: 5\n");
		printf("CRASH_AFTER_BALLOC: 6\n");
	} else {
		crash_pos = atoi(argv[1]);
		if (crash_pos < 0) {
			printf("Invalid crash position\n");
			exit(1);
		}
	}

	crfsinit(USE_DEFAULT_PARAM, USE_DEFAULT_PARAM, DEFAULT_SCHEDULER_POLICY);

	if ((fd = crfsopen(TESTDIR "/testfile", O_CREAT | O_RDWR, FILEPERM)) < 0) {
		perror("creat");
		printf("File create failure \n");
		exit(1);
	}

	/* Inject crash position */
	crfsinjectcrash(fd, crash_pos);

	printf("after injecting the code\n");

	/* Fill write buffer */
	memset(buf, 0x61, BLOCKSIZE);

	/* Write (supposed to crash at the position set above) */
	ret = crfswrite(fd, buf, BLOCKSIZE); 
	if (ret != BLOCKSIZE) {
		printf("Write failed as expected, returned with %d\n", ret);
	} 

	/* Write-and-checksum (supposed to crash at the position set above) */
	/* memset with some random data */
	memset(buf, 0x61 + i % 26, BLOCKSIZE);

	ret = crfschecksumwritecc(fd, buf, BLOCKSIZE); 
	if (ret != BLOCKSIZE) {
		printf("Write_and_checksum failed as expected, returned with %d\n", ret);
	} 

	crfsexit();

	printf("Benchmark completed \n");
	return 0;
}
