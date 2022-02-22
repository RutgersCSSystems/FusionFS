#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <dirent.h>
#include <pthread.h>

#include "../crc32/crc32_defs.h"

#define TESTDIR "/mnt/pmemdir"

#define BLOCKSIZE 16777216
//#define BLOCKSIZE 16384
#define FSPATHLEN 256
#define ITERS 100
#define FILEPERM 0666
#define DIRPERM 0755

#define MAGIC_SEED 0

pthread_t *threads = NULL;
int *thread_id = NULL;

double simulation_time(struct timeval start, struct timeval end ){
        double current_time;
        current_time = ((end.tv_sec*1000000*1.0 + end.tv_usec) -
                        (start.tv_sec*1000000*1.0 + start.tv_usec));
        return current_time;
}

void* do_recovery(void *arg) {
	char *buf = malloc(BLOCKSIZE);
	char fname[255];
	int fd = 0, i = 0, ret = 0, crc = 0;
	int id = *(int*)arg;

	sprintf(fname, "%s/testfile_%d", TESTDIR, id);
	fd = open(fname, O_RDWR, 0666);
	
	/* Read data from file */
	read(fd, buf, BLOCKSIZE-4);

	/* Calculate checksum */
	crc = crc32(MAGIC_SEED, buf, BLOCKSIZE-4);

	/* Restore checksum to file */
	write(fd, (char*)&crc, 4);

	close(fd);

	free(buf);
}

int main(int argc, char **argv) {
	int i, fd = 0, ret = 0;
	int crash_pos = 0;
	int config_thread_nr = 0;
	char fname[255];
	char *buf = malloc(BLOCKSIZE);
	struct timeval start_t, end_t;
	double usec = 0.0;

	if (argc < 2) {
		printf("Usage: ./crash_test <crash_pos> <thread_nr>\n");
		exit(1);
	} else {
		config_thread_nr = atoi(argv[1]);
		if (config_thread_nr < 0) {
			printf("Invalid thread number\n");
			exit(1);
		}
	}

	/* Create test files */
	for (i = 0; i < config_thread_nr; ++i) {
		sprintf(fname, "%s/testfile_%d", TESTDIR, i);
		memset(buf, 0x61 + i % 26, BLOCKSIZE-4);
		fd = open(fname, O_CREAT | O_WRONLY, 0666);
		write(fd, buf, BLOCKSIZE);
		close(fd);
	}
	free(buf);

	threads = malloc(config_thread_nr*sizeof(pthread_t));
	thread_id = malloc(config_thread_nr*sizeof(int));

	gettimeofday(&start_t, NULL);

	for (i = 0; i < config_thread_nr; ++i) {
		thread_id[i] = i;
		pthread_create(&threads[i], NULL, do_recovery, &thread_id[i]);
	}

	for (i = 0; i < config_thread_nr; ++i) {
		pthread_join(threads[i], NULL);
	}

	gettimeofday(&end_t, NULL);
	usec = simulation_time(start_t, end_t);

	printf("Recovery takes %lf us\n", usec);
	return 0;
}
