#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>
#include <pthread.h>
#include <sys/time.h>
#include <kcapi.h>

#include "../unvme_nvme.h"
#include "../fusionfslib.h"

#ifdef _POSIX
#define TESTDIR "/mnt/pmemdir"
//#define TESTDIR "/dev/shm"
#else
#define TESTDIR "/mnt/ram"
#endif

#define KB (1024UL)
#define MB (1024 * KB)
#define GB (1024 * MB)
#define FILEPERM 0666

#define CHECKSUMSIZE 4
#define MAGIC_SEED 0

/*
 * By default, thread # is fixed to 4
 * and block size (a.k.a value size) is 4096
 */
int thread_nr = 1;
int BLOCKSIZE = 256;
int ITERS = 0;
unsigned long FILESIZE = 4 * GB;

pthread_mutex_t g_lock;
double g_avgthput = 0;

pthread_t *tid;
int *thread_idx;
int *fd;

/* benchmark function pointer */
void* (*benchmark)(void*) = NULL;

/* To calculate simulation time */
double simulation_time(struct timeval start, struct timeval end ){
	double current_time;
	current_time = ((end.tv_sec + end.tv_usec * 1.0 / 1000000) -
			(start.tv_sec + start.tv_usec * 1.0 / 1000000));
	return current_time;
}

int aes_encrypt(void *inbuf, void *outbuf) {
	struct kcapi_handle *handle = NULL;
	uint8_t key[16];
	const uint8_t * iv = NULL;
	int itr = BLOCKSIZE / 16;

	if (kcapi_cipher_init(&handle, "cbc(aes)", 0)) {
		printf("Allocation of aes cipher failed\n");
		return 1;
	}

	memset(key, 0x61, 16);
	if (kcapi_cipher_setkey(handle, key, 16)) {
		printf("Symmetric cipher setkey failed \n");
		return 1;
	}	

	kcapi_cipher_encrypt(handle, (const uint8_t *)inbuf, BLOCKSIZE, iv, 
		(uint8_t *)outbuf, BLOCKSIZE, KCAPI_ACCESS_VMSPLICE);

	kcapi_cipher_destroy(handle);

	return 0;
}

#ifdef _POSIX
void* do_encryption_write(void* arg) {
	char *inbuf = malloc(BLOCKSIZE);
	char *outbuf = malloc(BLOCKSIZE);
	uint64_t i = 0, j = 0;
	int thread_id = *(int*)arg;
	double sec = 0.0;
    	double thruput = 0;
    	struct timeval start_t, end_t;

	gettimeofday(&start_t, NULL);
	for (i = 0; i < ITERS; i++) {
		/* Read the block here */
		if (pread(fd[thread_id], inbuf, BLOCKSIZE, i*BLOCKSIZE) != BLOCKSIZE) {
			printf("File data block failed to read \n");
			return NULL;
		}

		/* Perform the AES encryption */
		if (aes_encrypt(inbuf, outbuf)) {
			printf("AES encryption fail \n");
			return NULL;
		}

		/* Write back new data with checksum */
		if (pwrite(fd[thread_id], outbuf, BLOCKSIZE, i*BLOCKSIZE) != BLOCKSIZE) {
			printf("File data block checksum write fail \n");
			return NULL;
		}

	}

	gettimeofday(&end_t, NULL);
	sec = simulation_time(start_t, end_t);
	thruput = (double)(FILESIZE) / sec;

	pthread_mutex_lock(&g_lock);
	g_avgthput += thruput;
	pthread_mutex_unlock(&g_lock);

	free(inbuf);
	free(outbuf);
}


#else


void* do_encryption_write(void* arg) {
	char *buf = malloc(BLOCKSIZE);
	uint64_t i = 0;
	int thread_id = *(int*)arg;
	double sec = 0.0;
	double thruput = 0;
	struct timeval start_t, end_t;

	gettimeofday(&start_t, NULL);

#ifdef _OFFLOAD
	for (i = 0; i < ITERS; i++) {
		/* Issue a encryption-and-write directly, offload to storage */
		if (devfspreadencryptpwrite(fd[thread_id], buf, BLOCKSIZE, i*BLOCKSIZE) != BLOCKSIZE) {
			printf("File data block checksum write fail \n");
			return NULL;
		}
	}
#else
	char *inbuf = malloc(BLOCKSIZE);
	char *outbuf = malloc(BLOCKSIZE);

	for (i = 0; i < ITERS; i++) {
		/* Read the block here */
		if (crfspread(fd[thread_id], inbuf, BLOCKSIZE, i*BLOCKSIZE) != BLOCKSIZE) {
			printf("File data block failed to read \n");
			return NULL;
		}

		/* Perform the AES encryption */
		if (aes_encrypt(inbuf, outbuf)) {
			printf("AES encryption fail \n");
			return NULL;
		}

		/* Write back new data with checksum */
		if (crfspwrite(fd[thread_id], outbuf, BLOCKSIZE, i*BLOCKSIZE) != BLOCKSIZE) {
			printf("File data block checksum write fail \n");
			return NULL;
		}
	}
#endif
	gettimeofday(&end_t, NULL);
	sec = simulation_time(start_t, end_t);
	thruput = (double)(FILESIZE) / sec;

	pthread_mutex_lock(&g_lock);
	g_avgthput += thruput;
	pthread_mutex_unlock(&g_lock);

	free(buf);
}


#endif	//_POSIX

int main(int argc, char **argv) {
	uint64_t i = 0, j = 0, k = 0;
	int ret = 0;
	struct stat st;
	struct timeval start, end;
	double sec = 0.0;
	char fname[256];
	char *buf;

	if (argc < 3) {
		printf("invalid argument\n");
		printf("./test_encryption IO_size thread_count\n");
		return 0;
	}

	/* Get I/O size (value size) from input argument */
	BLOCKSIZE = atoi(argv[1]);
	thread_nr = atoi(argv[2]);
	FILESIZE  = FILESIZE / thread_nr;

	buf = (char*)malloc(BLOCKSIZE);
	ITERS = FILESIZE / BLOCKSIZE;

	fd = malloc(thread_nr*sizeof(int));

#ifndef _POSIX
	crfsinit(USE_DEFAULT_PARAM, USE_DEFAULT_PARAM, DEFAULT_SCHEDULER_POLICY);
#endif

	/* Step 1: Create Testing File */
	for (int f = 0; f < thread_nr; ++f) {
		sprintf(fname, "%s/testfile%d", TESTDIR, f);
#ifdef _POSIX
		if ((fd[f] = open(fname, O_CREAT | O_RDWR, FILEPERM)) < 0) {
#else
		if ((fd[f] = crfsopen(fname, O_CREAT | O_RDWR, FILEPERM)) < 0) {
#endif
			perror("creat");
			goto benchmark_exit;
		}
	}

	/* Step 2: Write to file */
	for (k = 0; k < thread_nr; ++k) {
		memset(buf, 0x61 + i % 26, BLOCKSIZE);
		for (i = 0; i < ITERS; ++i) {
			/* Write new content to this block */
			for (j = 0; j < BLOCKSIZE; j++) {
				buf[j] = 0x41 + j % 26;
			}
#ifdef _POSIX
			if (write(fd[k], buf, BLOCKSIZE) != BLOCKSIZE) {
#else
			if (crfswrite(fd[k], buf, BLOCKSIZE) != BLOCKSIZE) {
#endif
				printf("File data block write fail \n");
				goto benchmark_exit;
			}
		}
	}
	printf("finish writing %lu blocks\n", i);

	tid = malloc(thread_nr*sizeof(pthread_t));
	thread_idx = malloc(thread_nr*sizeof(int));
	pthread_mutex_init(&g_lock, NULL);

	/* Start timing checksum write */
	gettimeofday(&start, NULL);
	
	/* Step 3: Run benchmark */
	benchmark = &do_encryption_write;
	for (i = 0; i < thread_nr; ++i) {
		thread_idx[i] = i;
		pthread_create(&tid[i], NULL, benchmark, &thread_idx[i]);
	}

	for (i = 0; i < thread_nr; ++i)
		pthread_join(tid[i], NULL);

	gettimeofday(&end, NULL);
	sec = simulation_time(start, end);
	printf("Benchmark takes %.2lf s, average thruput %lf B/s\n", sec, g_avgthput );
	printf("Benchmark takes %.2lf s, average thruput %.2lf GB/s\n", sec, g_avgthput / 1024 / 1024/ 1024);

	printf("Benchmark completed \n");

benchmark_exit:

#ifdef _POSIX
	for (int f = 0; f < thread_nr; ++f)
		close(fd[f]);
#else
	for (int f = 0; f < thread_nr; ++f)
		crfsclose(fd[f]);
	crfsexit();
#endif

	free(fd);
	free(tid);
	free(thread_idx);
	free(buf);

	return 0;
}
