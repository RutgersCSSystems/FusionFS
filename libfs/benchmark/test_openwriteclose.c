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

int thread_nr = 4;
int BLOCKSIZE = 4096;
int ITERS = 0;
unsigned long FILESIZE = 0;
int FILENUM = 0;
int batch_size = 5;

pthread_mutex_t g_lock;
double g_avgthput = 0;

pthread_t *tid;
int *thread_idx;

/* To calculate simulation time */
double simulation_time(struct timeval start, struct timeval end) {
        double current_time;
        current_time = ((end.tv_sec + end.tv_usec * 1.0 / 1000000) -
                        (start.tv_sec + start.tv_usec * 1.0 / 1000000));
        return current_time;
}

int gen_test_file(char *filename, int file_size) {
        int fd = 0;
        char *buf = (char *)malloc(BLOCKSIZE);
        uint64_t i = 0;

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
        for (i = 0; i < file_size / BLOCKSIZE; i++) {
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

#ifndef COMPOUND
void *do_open_write_close(void *arg) {
        int fd = 0;
        char filearr[512];
        struct stat st;
        char read_dir[256];

        struct dirent *entry = NULL;
        double thruput = 0;
        struct timeval start_t, end_t;
        double sec = 0.0;
        size_t outsz = 0;
        int thread_id = *(int *)arg;

        snprintf(read_dir, sizeof(read_dir), "%s%d", INPUT_DIR, thread_id);
        DIR *mydir = opendir(read_dir);
        assert(mydir);
        entry = readdir(mydir);
        assert(entry);
        char *buf = malloc(BLOCKSIZE);

        gettimeofday(&start_t, NULL);
        while ((entry = readdir(mydir)) != NULL) {
                char *output = NULL;
                char *input = NULL;
                fd = 0;

                if (entry->d_type == DT_DIR) continue;

                if (strlen(entry->d_name) < 4) continue;

                bzero(filearr, 512);
                strcpy(filearr, read_dir);
                strcat(filearr, "/");
                strcat(filearr, entry->d_name);

                stat(filearr, &st);
                if (st.st_size == 0) continue;
#ifdef _POSIX
                if ((fd = open(filearr, O_RDWR, 0666)) < 0) {
#else
                if ((fd = crfsopen(filearr, O_RDWR, 0666)) < 0) {
#endif
                        perror("open file");
                        return NULL;
                }

                for (int i = 0; i < FILESIZE / BLOCKSIZE; i++) {
                        memset(buf, 0x61 + i % 26, BLOCKSIZE);
#ifdef _POSIX
                        if (pwrite(fd, buf, BLOCKSIZE, i * BLOCKSIZE) !=
                            BLOCKSIZE) {
#else
                        if (crfspwrite(fd, buf, BLOCKSIZE, i * BLOCKSIZE) !=
                            BLOCKSIZE) {
#endif
                                printf(
                                    "File data block checksum write fail \n");
                                if (fd > 0)
#ifdef _POSIX
                                        close(fd);
#else
                                        crfsclose(fd);
#endif
                                return NULL;
                        }
                }
                
                if (fd > 0)
#ifdef _POSIX
                        close(fd);
#else
                        crfsclose(fd);
#endif
        }
        gettimeofday(&end_t, NULL);
        sec = simulation_time(start_t, end_t);
        thruput = (double)(FILESIZE * FILENUM / thread_nr) / sec;

        pthread_mutex_lock(&g_lock);
        g_avgthput += thruput;
        pthread_mutex_unlock(&g_lock);
        printf("time: %.2lf, Average open write close throughput: %.2lf MB/s\n",
               sec, thruput / 1024 / 1024);
        free(buf);
}
#else
void *do_open_write_close(void *arg) {
        int fd = 0;
        char filearr[512];
        struct stat st;
        char read_dir[256];

        struct dirent *entry = NULL;
        double thruput = 0;
        struct timeval start_t, end_t;
        double sec = 0.0;
        size_t outsz = 0;
        int thread_id = *(int *)arg;
        int tmp_fd = 0;

        snprintf(read_dir, sizeof(read_dir), "%s%d", INPUT_DIR, thread_id);
        printf("open_write_close offload, %s\n", read_dir);
        DIR *mydir = opendir(read_dir);
        assert(mydir);
        entry = readdir(mydir);
        assert(entry);
        char *buf = malloc(FILESIZE);
        char test_file[256];
        bzero(test_file, 256);
        snprintf(test_file, sizeof(test_file), "%s%d", TESTDIR "/testfile",
                 thread_id);
        if ((tmp_fd = crfsopen(test_file, O_CREAT | O_RDWR, FILEPERM)) < 0) {
                perror("creat");
                return NULL;
        }

        gettimeofday(&start_t, NULL);
        for (int j = 0; j < FILENUM / thread_nr; j++) {
                char filename[256];
                snprintf(filename, sizeof(filename), "%s/%s%d", read_dir,
                         "testfile", j);

                memset(buf, 0x61 + j % 26, FILESIZE);

                if (devfsopenpwriteclose(tmp_fd, buf, FILESIZE, 0, filename) !=
                    FILESIZE) {
                        printf("File data block read fail \n");
                        return NULL;
                }
        }
        gettimeofday(&end_t, NULL);
        sec = simulation_time(start_t, end_t);
        thruput = (double)(FILESIZE * FILENUM / thread_nr) / sec;

        pthread_mutex_lock(&g_lock);
        g_avgthput += thruput;
        pthread_mutex_unlock(&g_lock);
        printf("time: %.2lf, Average open write close throughput: %.2lf MB/s\n",
               sec, thruput / 1024 / 1024);
        free(buf);
        if (tmp_fd > 0) {
                crfsclose(tmp_fd);
        }
}
#endif
int main(int argc, char **argv) {
        int i = 0, ret = 0;
        struct stat st;
        struct timeval start, end;
        double sec = 0.0;
        char test_file[256];
        char read_dir[256];
        int benchmark_type = 0;
        if (argc < 4) {
                printf("invalid argument\n");
                printf(
                    "./test_openwritelose file_num file_size(MB) "
                    "thread_count\n");
                return 0;
        }
        /* Parse input argument */
        FILENUM = atoi(argv[1]);
        FILESIZE = atof(argv[2]) * MB;
        thread_nr = atoi(argv[3]);

#ifndef _POSIX
        crfsinit(USE_DEFAULT_PARAM, USE_DEFAULT_PARAM,
                 DEFAULT_SCHEDULER_POLICY);
#endif

        /* Step 1: Create Testing File */
        for (i = 0; i < thread_nr; i++) {
                bzero(read_dir, 256);
                snprintf(read_dir, sizeof(read_dir), "%s%d", INPUT_DIR, i);
                mkdir(read_dir, 0755);
                for (int j = 0; j < FILENUM / thread_nr; j++) {
                        char filename[256];
                        snprintf(filename, sizeof(filename), "%s/%s%d",
                                 read_dir, "testfile", j);
                        gen_test_file(filename, FILESIZE);
                }
                printf("finish creating %u files, thread_id: %d\n",
                       FILENUM / thread_nr, i);
        }

        tid = malloc(thread_nr * sizeof(pthread_t));
        thread_idx = malloc(thread_nr * sizeof(int));

        pthread_mutex_init(&g_lock, NULL);

        /* Start timing checksum write */
        gettimeofday(&start, NULL);

        /* Step 3: Run benchmark */
        for (i = 0; i < thread_nr; ++i) {
                thread_idx[i] = i;
                pthread_create(&tid[i], NULL, do_open_write_close,
                               &thread_idx[i]);
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

#ifndef _POSIX
        crfsexit();
#endif

        return 0;
}
