#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../crc32/crc32_defs.h"
#include "../unvme_nvme.h"
#include "../fusionfslib.h"

#ifdef _POSIX
#define TESTDIR "/mnt/pmemdir"
//#define TESTDIR "/dev/shm"
#define INPUT_DIR "/mnt/pmemdir/dataset"
#else
#define TESTDIR "/mnt/ram"
#define INPUT_DIR "/mnt/ram/dataset"
#endif

#define KB (1024UL)
#define MB (1024 * KB)
#define GB (1024 * MB)
#define FILEPERM 0666

#define CHECKSUMSIZE 4
#define MAGIC_SEED 0

#define READ_MODIFY_WRITE_SYS_NUM 548
#define WRITE_CHECKSUM_SYS_NUM 549

/*
 * By default, thread # is fixed to 4
 * and block size (a.k.a value size) is 4096
 */
int thread_nr = 4;
int BLOCKSIZE = 4096;
int ITERS = 0;
unsigned long FILESIZE = 2 * GB;
int FILENUM = 6000;
int batch_size = 5;

pthread_mutex_t g_lock;
double g_avgthput = 0;

pthread_t *tid;
int *thread_idx;

/* benchmark function pointer */
void *(*benchmark)(void *) = NULL;

/* To calculate simulation time */
double simulation_time(struct timeval start, struct timeval end) {
        double current_time;
        current_time = ((end.tv_sec + end.tv_usec * 1.0 / 1000000) -
                        (start.tv_sec + start.tv_usec * 1.0 / 1000000));
        return current_time;
}

#ifndef COMPOUND
void *do_checksum_write(void *arg) {
        char *buf = malloc(BLOCKSIZE);
        uint64_t i = 0, j = 0;
        int fd = 0;
        int thread_id = *(int *)arg;
        int range = ITERS / thread_nr;
        double start = thread_id * range;
        double end = (thread_id + 1) * range;
        uint32_t crc = 0;
        double sec = 0.0;
        double thruput = 0;
        struct timeval start_t, end_t;

        if ((fd = crfsopen(TESTDIR "/testfile", O_RDWR, FILEPERM)) < 0) {
                perror("creat");
                return NULL;
        }

        gettimeofday(&start_t, NULL);

        for (i = start; i < end; i++) {
                /* Write new content to this block */
                for (int j = 0; j < BLOCKSIZE; j++) {
                        buf[j] = 0x41 + j % 26;
                }

                /* Calculate checksum */
                crc = crc32(MAGIC_SEED, buf, BLOCKSIZE - CHECKSUMSIZE);

                /* Put checksum into write block */
                memcpy(buf + BLOCKSIZE - CHECKSUMSIZE, (void *)&crc,
                       CHECKSUMSIZE);

#ifndef WRITE_SEPARATE
                /* Write back new data with checksum */
                if (crfspwrite(fd, buf, BLOCKSIZE, i * BLOCKSIZE) !=
                    BLOCKSIZE) {
                        printf("File data block checksum write fail \n");
                        return NULL;
                }

#else
                /* Write back new data */
                if (crfspwrite(fd, buf, BLOCKSIZE - CHECKSUMSIZE,
                               i * BLOCKSIZE) != BLOCKSIZE - CHECKSUMSIZE) {
                        printf("File data block write fail \n");
                        return NULL;
                }

                /* Write back checksum */
                if (crfspwrite(fd, buf + BLOCKSIZE - CHECKSUMSIZE, CHECKSUMSIZE,
                               i * BLOCKSIZE + BLOCKSIZE - CHECKSUMSIZE) !=
                    CHECKSUMSIZE) {
                        printf("File data block checksum fail \n");
                        return NULL;
                }

#endif
        }

        gettimeofday(&end_t, NULL);
        sec = simulation_time(start_t, end_t);
        thruput = (double)((end - start) * BLOCKSIZE) / sec;

        pthread_mutex_lock(&g_lock);
        g_avgthput += thruput;
        pthread_mutex_unlock(&g_lock);

        crfsclose(fd);
        free(buf);
}

void *do_read_modify_write(void *arg) {
        char *buf = malloc(BLOCKSIZE);
        uint64_t i = 0, j = 0;
        int fd = 0;
        int thread_id = *(int *)arg;
        int range = ITERS / thread_nr;
        double start = thread_id * range;
        double end = (thread_id + 1) * range;
        uint32_t crc = 0;
        double sec = 0.0;
        double thruput = 0;
        struct timeval start_t, end_t;
#ifndef _POSIX
        if ((fd = crfsopen(TESTDIR "/testfile", O_RDWR, FILEPERM)) < 0) {
#else
        if ((fd = open(TESTDIR "/testfile", O_RDWR, FILEPERM)) < 0) {
#endif
                perror("creat");
                return NULL;
        }

        gettimeofday(&start_t, NULL);

        for (i = start; i < end; i++) {
/* Read entire block */
#ifndef _POSIX
                if (crfspread(fd, buf, BLOCKSIZE, i * BLOCKSIZE) != BLOCKSIZE) {
#else
                if (pread(fd, buf, BLOCKSIZE, i * BLOCKSIZE) != BLOCKSIZE) {
#endif
                        printf("File data block read fail \n");
                        return NULL;
                }

                /* Write new content to this block */
                for (int j = 0; j < BLOCKSIZE; j++) {
                        buf[j] = 0x41 + j % 26;
                }

#ifndef WRITE_SEPARATE
                /* Write back new data with checksum */
                if (crfspwrite(fd, buf, BLOCKSIZE, i * BLOCKSIZE) !=
                    BLOCKSIZE) {
                        printf("File data block checksum write fail \n");
                        return NULL;
                }
#else
/* Write back new data */
#ifndef _POSIX
                if (crfspwrite(fd, buf, BLOCKSIZE - CHECKSUMSIZE,
                               i * BLOCKSIZE) != BLOCKSIZE - CHECKSUMSIZE) {
#else
                if (pwrite(fd, buf, BLOCKSIZE - CHECKSUMSIZE, i * BLOCKSIZE) !=
                    BLOCKSIZE - CHECKSUMSIZE) {
#endif
                        printf("File data block write fail \n");
                        return NULL;
                }

/* Write back checksum */
#ifndef _POSIX
                if (crfspwrite(fd, buf + BLOCKSIZE - CHECKSUMSIZE, CHECKSUMSIZE,
                               i * BLOCKSIZE + BLOCKSIZE - CHECKSUMSIZE) !=
                    CHECKSUMSIZE) {
#else
                if (pwrite(fd, buf + BLOCKSIZE - CHECKSUMSIZE, CHECKSUMSIZE,
                           i * BLOCKSIZE + BLOCKSIZE - CHECKSUMSIZE) !=
                    CHECKSUMSIZE) {
#endif
                        printf("File data block checksum fail \n");
                        return NULL;
                }
#endif
        }

        gettimeofday(&end_t, NULL);
        sec = simulation_time(start_t, end_t);
        thruput = (double)((end - start) * BLOCKSIZE) / sec;

        pthread_mutex_lock(&g_lock);
        g_avgthput += thruput;
        pthread_mutex_unlock(&g_lock);

#ifndef _POSIX
        crfsclose(fd);
#else
        close(fd);
#endif
        free(buf);
}

#else
void *do_checksum_write(void *arg) {
        char *buf = malloc(BLOCKSIZE);
        uint64_t i = 0;
        int fd = 0;
        int thread_id = *(int *)arg;
        int range = ITERS / thread_nr;
        double start = thread_id * range;
        double end = (thread_id + 1) * range;
        double sec = 0.0;
        double thruput = 0;
        struct timeval start_t, end_t;

#ifdef _POSIX
        if ((fd = open(TESTDIR "/testfile", O_RDWR, FILEPERM)) < 0) {
#else
        if ((fd = crfsopen(TESTDIR "/testfile", O_RDWR, FILEPERM)) < 0) {
#endif
                perror("creat");
                return NULL;
        }

        gettimeofday(&start_t, NULL);
        for (i = start; i < end; i++) {
                /* Write new content to this block */
                for (int j = 0; j < BLOCKSIZE; j++) {
                        buf[j] = 0x41 + j % 26;
                }
#ifdef _POSIX
                if (syscall(WRITE_CHECKSUM_SYS_NUM, fd, buf, BLOCKSIZE,
                            i * BLOCKSIZE) != BLOCKSIZE) {
                        printf("File data block checksum write fail \n");
                        return NULL;
                }
#else
                /* Issue a checksum pwrite directly, offload to storage */
                if (devfschecksumpwrite(fd, buf, BLOCKSIZE, i * BLOCKSIZE) !=
                    BLOCKSIZE) {
                        printf("File data block checksum write fail \n");
                        return NULL;
                }
#endif
        }
        gettimeofday(&end_t, NULL);
        sec = simulation_time(start_t, end_t);
        thruput = (double)((end - start) * BLOCKSIZE) / sec;

        pthread_mutex_lock(&g_lock);
        g_avgthput += thruput;
        pthread_mutex_unlock(&g_lock);

#ifdef _POSIX
        close(fd);
#else
        crfsclose(fd);
#endif
        free(buf);
}

void *do_read_modify_write(void *arg) {
        char *buf = malloc(BLOCKSIZE);
        uint64_t i = 0;
        int fd = 0;
        int thread_id = *(int *)arg;
        int range = ITERS / thread_nr;
        double start = thread_id * range;
        double end = (thread_id + 1) * range;
        double sec = 0.0;
        double thruput = 0;
        struct timeval start_t, end_t;
#ifdef _POSIX
        if ((fd = open(TESTDIR "/testfile", O_RDWR, FILEPERM)) < 0) {
#else
        if ((fd = crfsopen(TESTDIR "/testfile", O_RDWR, FILEPERM)) < 0) {
#endif
                perror("creat");
                return NULL;
        }

        gettimeofday(&start_t, NULL);
        for (i = start; i < end; i++) {
                /* Write new content to this block */
                for (int j = 0; j < BLOCKSIZE; j++) {
                        buf[j] = 0x41 + j % 26;
                }

                /* Issue a read-modify-write directly, offload to storage */
#ifdef _POSIX
                if (syscall(READ_MODIFY_WRITE_SYS_NUM, fd, buf, BLOCKSIZE,
                            i * BLOCKSIZE) != BLOCKSIZE) {
#else
                if (devfsreadmodifywrite(fd, buf, BLOCKSIZE, i * BLOCKSIZE) !=
                    BLOCKSIZE) {
#endif
                        printf("File data block read modify write fail \n");
                        return NULL;
                }
        }
        gettimeofday(&end_t, NULL);
        sec = simulation_time(start_t, end_t);
        thruput = (double)((end - start) * BLOCKSIZE) / sec;

        pthread_mutex_lock(&g_lock);
        g_avgthput += thruput;
        pthread_mutex_unlock(&g_lock);
#ifdef _POSIX
        close(fd);
#else
        crfsclose(fd);
#endif
        free(buf);
}

#endif

int main(int argc, char **argv) {
        uint64_t i = 0;
        int fd = 0, ret = 0;
        uint32_t crc = 0;
        struct stat st;
        struct timeval start, end;
        double sec = 0.0;
        char *buf;
        char test_file[256];
        char read_dir[256];
        int benchmark_type = 0;
        if (argc < 4) {
                printf("invalid argument\n");
                printf(
                    "./test_checksum benchmark_type(1 and 2) IO_size "
                    "thread_count <file_size (MB)>\n");
                return 0;
        }
        /* Get benchmark type from input argument */
        benchmark_type = atoi(argv[1]);
        switch (benchmark_type) {
                case 1:
                        benchmark = &do_checksum_write;
                        break;

                case 2:
                        benchmark = &do_read_modify_write;
                        break;

                default:
                        benchmark = &do_checksum_write;
                        break;
        }

        /* Get I/O size (value size) from input argument */
        BLOCKSIZE = atoi(argv[2]);
        thread_nr = atoi(argv[3]);
        if (argc >= 5) {
                FILESIZE = atoi(argv[4]) * MB;
        }

        buf = (char *)malloc(BLOCKSIZE);
        ITERS = FILESIZE / BLOCKSIZE;
#ifndef _POSIX
        crfsinit(USE_DEFAULT_PARAM, USE_DEFAULT_PARAM,
                 DEFAULT_SCHEDULER_POLICY);
#endif

#ifndef _POSIX
        /* Step 1: Create Testing File */
        if ((fd = crfsopen(TESTDIR "/testfile", O_CREAT | O_RDWR, FILEPERM)) <
            0) {
#else
        if ((fd = open(TESTDIR "/testfile", O_CREAT | O_RDWR, FILEPERM)) < 0) {
#endif
                perror("creat");
                goto benchmark_exit;
        }

        /* Step 2: Append blocks to storage with random contents and checksum
         */
        for (i = 0; i < ITERS; i++) {
                /* memset with some random data */
                memset(buf, 0x61 + i % 26, BLOCKSIZE);

#ifndef COMPOUND
                /* Calculate checksum */
                crc = crc32(MAGIC_SEED, buf, BLOCKSIZE - CHECKSUMSIZE);

                /* Put checksum into write block */
                memcpy(buf + BLOCKSIZE - CHECKSUMSIZE, (void *)&crc,
                       CHECKSUMSIZE);
#ifndef _POSIX
                if (crfswrite(fd, buf, BLOCKSIZE) != BLOCKSIZE) {
#else
                if (write(fd, buf, BLOCKSIZE) != BLOCKSIZE) {
#endif
                        printf("File data block write fail \n");
                        goto benchmark_exit;
                }
#else
#ifndef _POSIX
                if (devfschecksumwrite(fd, buf, BLOCKSIZE) != BLOCKSIZE) {
                        printf("File data block checksum write fail \n");
                        goto benchmark_exit;
                }
#else
                if (syscall(WRITE_CHECKSUM_SYS_NUM, fd, buf, BLOCKSIZE,
                            i * BLOCKSIZE) != BLOCKSIZE) {
                        printf("File data block checksum write fail \n");
                        goto benchmark_exit;
                }
#endif
#endif
        }
        printf("finished writing\n");

        tid = malloc(thread_nr * sizeof(pthread_t));
        thread_idx = malloc(thread_nr * sizeof(int));

        pthread_mutex_init(&g_lock, NULL);

        /* Start timing checksum write */
        gettimeofday(&start, NULL);

        /* Step 3: Run benchmark */
        for (i = 0; i < thread_nr; ++i) {
                thread_idx[i] = i;
                pthread_create(&tid[i], NULL, benchmark, &thread_idx[i]);
        }

        for (i = 0; i < thread_nr; ++i) pthread_join(tid[i], NULL);

        gettimeofday(&end, NULL);
        sec = simulation_time(start, end);
        printf("Benchmark takes %.2lf s, average thruput %.2lf GB/s\n", sec,
               g_avgthput / 1024 / 1024 / 1024);

        printf("Benchmark completed \n");

benchmark_exit:
        free(tid);
        free(thread_idx);
        free(buf);

#ifndef _POSIX
        crfsclose(fd);
        crfsexit();
#else
        close(fd);
#endif

        return 0;
}
