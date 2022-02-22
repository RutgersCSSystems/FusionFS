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
#include <assert.h>
#include <math.h>

#include "../crc32/crc32_defs.h"
#include "../unvme_nvme.h"
#include "../fusionfslib.h"

#define TESTDIR "/mnt/ram"
#define INPUT_DIR "/mnt/ram/dataset"
#define OUTPUT_DIR "/mnt/ram/output_dir/"
#define USER

#define KB (1024UL)
#define MB (1024 * KB)
#define GB (1024 * MB)
#define FILEPERM 0666

#define CHECKSUMSIZE 4
#define MAGIC_SEED 0

#define CPU_FAIR 1
#define MEMORY_FAIR 2

static int g_snappy_init = 0;
/*
 * By default, thread # is fixed to 4
 * and block size (a.k.a value size) is 4096
 */
int thread_nr = 2;
int thread_nr1 = 0;
int thread_nr2 = 0;
int chk_thread_nr = 0;
int BLOCKSIZE = 4096;
int ITERS = 0;
int FILENUM = 6;
int benchmark_type = 0;
unsigned long FILESIZE = 100 * MB;

pthread_mutex_t g_lock;
double g_avgthput_compress = 0;
double g_avgthput_checksum = 0;
double g_avgthput_write = 0;

double thruput_compress[32];
double thruput_checksum[32];
double thruput_write[32];

pthread_t *tid;
int *thread_idx;

size_t g_tot_input_bytes = 0;
size_t g_tot_output_bytes = 0;
char g_buf[4096];
double compress_time=0;

/* benchmark function pointer */
void* (*benchmark1)(void*) = NULL;
void* (*benchmark2)(void*) = NULL;

/* To calculate simulation time */
double simulation_time(struct timeval start, struct timeval end ){
	double current_time;
	current_time = ((end.tv_sec + end.tv_usec * 1.0 / 1000000) -
			(start.tv_sec + start.tv_usec * 1.0 / 1000000));
	return current_time;
}

double calculateSD(double data[], double mean, int num){
	double sum = 0.0, standardDeviation = 0.0;
	int i;

	for(i=0; i<num; ++i)
		standardDeviation += pow(data[i] - mean, 2);

	return sqrt(standardDeviation/num);
}

int gen_test_file(char* filename, int iters) {
    int fd = 0;
    char* buf = (char*)malloc(BLOCKSIZE);
    uint64_t i = 0;
    uint32_t crc = 0;

    /* Step 1: Create Testing File */

#ifndef _POSIX
    if ((fd = crfsopen(filename, O_CREAT | O_RDWR, FILEPERM)) < 0) {
#else
    if ((fd = open(filename, O_CREAT | O_RDWR, FILEPERM)) < 0) {
#endif
        perror("creat");
        goto gen_file_failed;
    }

    /* Step 2: Append blocks to storage with random contents and checksum */
    for (i = 0; i < iters; i++) {
        /* memset with some random data */
        memset(buf, 0x61 + i % 26, BLOCKSIZE);

#ifndef _POSIX
        if (crfswrite(fd, buf, BLOCKSIZE) != BLOCKSIZE) {
#else
        if (write(fd, buf, BLOCKSIZE) != BLOCKSIZE) {
#endif
            printf("File data block write fail \n");
            goto gen_file_failed;
        }
    }
#ifndef _POSIX
    crfsclose(fd);
#else
    close(fd);
#endif
    return fd;

gen_file_failed:
    free(buf);
    return -1;
}

void* do_checksum_write(void* arg) {
	char *buf = malloc(BLOCKSIZE);
	uint64_t i = 0;
	int fd = 0;
	int thread_id = *(int*)arg;
	int range = ITERS / chk_thread_nr;
	double start = thread_id*range;
        double end = (thread_id + 1)*range;
        double sec = 0.0;
        double thruput = 0;
        struct timeval start_t, end_t;
	char test_file[256];
	if (benchmark_type  == CPU_FAIR) {
        	snprintf(test_file, sizeof(test_file), "%s%d", TESTDIR "/benchmark1_", thread_id);
	} else {
        	snprintf(test_file, sizeof(test_file), "%s%d", TESTDIR "/benchmark2_", thread_id);
	}

	if ((fd = crfsopen(test_file, O_RDWR, FILEPERM)) < 0) {
		perror("creat");
		return NULL;
	}

        gettimeofday(&start_t, NULL);
        for (i = 0; i < ITERS; i++) {
		/* Write new content to this block */
		for (int j = 0; j < BLOCKSIZE; j++) {
			buf[j] = 0x41 + j % 26;
		}

		/* Issue a checksum pwrite directly, offload to storage */
		if (devfschecksumwrite(fd, buf, BLOCKSIZE) != BLOCKSIZE) {
			printf("File data block checksum write fail \n");
			return NULL;
		}
        }
        gettimeofday(&end_t, NULL);
        sec = simulation_time(start_t, end_t);
        thruput = (double)(ITERS * BLOCKSIZE) / sec / 1024 / 1024;

        pthread_mutex_lock(&g_lock);
        g_avgthput_checksum += thruput;
        thruput_checksum[thread_id] = thruput;
        pthread_mutex_unlock(&g_lock);
        printf("checksum write finished, fd: %d, thruput: %.2f MB/s\n", fd, thruput);
        crfsclose(fd);
	free(buf);
}

void* do_rand_write(void* arg) {
	u64 offset = 0;
	u64 i;
	int q, fd = 0;
	double sec = 0.0;
	double thruput = 0;
	struct timeval start, end;
	char test_file[256];
	int thread_id = *(int*)arg;
        snprintf(test_file, sizeof(test_file), "%s%d", TESTDIR "/benchmark2_", thread_id);
        srand(time(NULL));

	if ((fd = crfsopen(test_file, O_RDWR, FILEPERM)) < 0) {
		perror("creat");
		return NULL;
	}

	char* p = malloc(BLOCKSIZE);
	if (!p) {
		printf("user buffer malloc failed\n");
		return 0;
	}
	memset(p, 0, BLOCKSIZE);
	gettimeofday(&start, NULL);

	/* perform IO write */
	for (i = 0; i < ITERS; i++) {
		offset = (rand() % (FILESIZE - BLOCKSIZE));

		memset(p, 'a' + offset % 26, BLOCKSIZE);

                if (crfspwrite(fd, p, BLOCKSIZE, offset) != BLOCKSIZE) {
			printf("write failed \n");
			break;
		}
	}

	gettimeofday(&end, NULL);
	sec = simulation_time(start, end);
	thruput = (double)(i * BLOCKSIZE) / sec / 1024/1024;

	pthread_mutex_lock(&g_lock);
	g_avgthput_write += thruput;
        thruput_write[thread_id] = thruput;
	pthread_mutex_unlock(&g_lock);

	crfsclose(fd);
	free(p);
        printf("random write finished, fd: %d, thruput: %.2f MB/s\n", fd, thruput);
}

int main(int argc, char **argv) {
	int i = 0, j = 0;
	int fd = 0, ret = 0;
        uint32_t crc = 0;
        struct stat st;
        struct timeval start, end;
        double sec = 0.0;
        char *buf;
        char read_dir[256];
        char test_file[256];

	if (argc < 5) {
		printf("invalid argument\n");
		printf("./test_mm_scheduler benchmark_type IO_size io_thread_count compute_thread_count\n");
		printf("benchmark_type: 1, test CPU fairness \n");
		return 0;
	}

	/* Get I/O size (value size) from input argument */
        benchmark_type = atoi(argv[1]);
        BLOCKSIZE = atoi(argv[2]);
        thread_nr1 = atoi(argv[3]);
        thread_nr2 = atoi(argv[4]);
        thread_nr = thread_nr1 + thread_nr2;

        switch(benchmark_type) {
                case 1:
                        benchmark1 = &do_checksum_write;
                        benchmark2 = &do_rand_write;
                        chk_thread_nr = thread_nr1;
			break;
        }

	buf = (char*)malloc(BLOCKSIZE);
	ITERS = FILESIZE / BLOCKSIZE;

	crfsinit(USE_DEFAULT_PARAM, USE_DEFAULT_PARAM, DEFAULT_SCHEDULER_POLICY);

	/* Create Testing File for benchmark1 */
        if (benchmark_type == CPU_FAIR) {
                for (i = 0; i < thread_nr1; i ++) {
                        bzero(test_file, 256);
                        snprintf(test_file, sizeof(test_file), "%s%d", TESTDIR "/benchmark1_", i);
                        gen_test_file(test_file, ITERS);
                        printf("finish writing %u blocks, benchmark1\n", ITERS);

                } 
        }

	/* Create Testing File for benchmark2 */
        for (i = 0; i < thread_nr2; i ++) {
                bzero(test_file, 256);
                snprintf(test_file, sizeof(test_file), "%s%d", TESTDIR "/benchmark2_", i);
                gen_test_file(test_file, ITERS);
                printf("finish writing %u blocks, benchmark2\n", ITERS);
        }

	tid = malloc(thread_nr*sizeof(pthread_t));
        thread_idx = malloc(thread_nr*sizeof(int));
        pthread_mutex_init(&g_lock, NULL);

	/* Step 3: Run benchmark */
        gettimeofday(&start, NULL);

        for (i = 0; i < thread_nr1; ++i) {
		thread_idx[i] = i;
		pthread_create(&tid[i], NULL, benchmark1, &thread_idx[i]);
	}

	for (i = 0, j = thread_nr1; i < thread_nr2; ++i, j++) {
		thread_idx[j] = i;
		pthread_create(&tid[j], NULL, benchmark2, &thread_idx[j]);
	}

	for (i = 0; i < thread_nr; ++i)
		pthread_join(tid[i], NULL);

        gettimeofday(&end, NULL);
        sec = simulation_time(start, end);

        printf("Random write Benchmark, avg thruput %lf MB/s, std %lf MB/s\n", 
                        g_avgthput_write / thread_nr2, calculateSD(thruput_write, g_avgthput_write / thread_nr2, thread_nr2));
        printf("Checksum write Benchmark, avg thruput %lf MB/s, std %lf MB/s\n", 
                                g_avgthput_checksum / chk_thread_nr, calculateSD(thruput_checksum, g_avgthput_checksum / chk_thread_nr, chk_thread_nr));

        printf("Benchmark completed \n");

benchmark_exit:
	free(tid);
	free(thread_idx);
	free(buf);

	crfsexit();
	return 0;
}
