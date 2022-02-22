all: test_micro test_micro_offload test_micro_posix_compound test_micro_posix test_openwriteclose_offload test_openwriteclose_posix test_scheduler test_encryption test_encryption_posix test_encryption_crfs test_crash test_crash_multi test_recovery_ext4 crfs_exit

CODE=.
CXX=g++
CC=gcc
LIB_S=-lpthread -lrt -L../ -lfusionfslib -L./libfs/interval_tree -lintervaltreelib

test_micro: test_micro.c
	$(CC) $(INCLUDE) $(FLAGS) -DVALUE_SIZE -DWRITE_SEPARATE test_micro.c ../crc32/crc32.c -o test_micro $(LIB_S)

test_micro_offload: test_micro.c
	$(CC) $(INCLUDE) $(FLAGS) -DVALUE_SIZE -DCOMPOUND test_micro.c ../crc32/crc32.c -o test_micro_offload $(LIB_S)

test_micro_posix_compound: test_micro.c
	$(CC) $(INCLUDE) $(FLAGS) -DVALUE_SIZE -DCOMPOUND -D_POSIX test_micro.c ../crc32/crc32.c -o test_micro_posix_compound $(LIB_S)

test_micro_posix: test_micro.c
	$(CC) $(INCLUDE) $(FLAGS) -DVALUE_SIZE  -DWRITE_SEPARATE -D_POSIX test_micro.c ../crc32/crc32.c -o test_micro_posix $(LIB_S)

test_openwriteclose_offload: test_openwriteclose.c
	$(CC) $(INCLUDE) $(FLAGS) -DCOMPOUND test_openwriteclose.c -o test_openwriteclose_offload $(LIB_S)

test_openwriteclose_posix: test_openwriteclose.c
	$(CC) $(INCLUDE) $(FLAGS) -D_POSIX test_openwriteclose.c -o test_openwriteclose_posix $(LIB_S)

test_scheduler: test_scheduler.c
	$(CC) $(INCLUDE) $(FLAGS) test_scheduler.c ../crc32/crc32.o -o test_scheduler $(LIB_S) -lm

test_encryption: test_encryption.c
	$(CC) $(INCLUDE) -g -D_OFFLOAD test_encryption.c -o test_encryption $(LIB_S) -lkcapilib

test_encryption_posix: test_encryption.c
	$(CC) $(INCLUDE) -g -D_POSIX test_encryption.c -o test_encryption_posix $(LIB_S) -lkcapilib

test_encryption_crfs: test_encryption.c
	$(CC) $(INCLUDE) -g test_encryption.c -o test_encryption_crfs $(LIB_S) -lkcapilib

test_crash: test_crash.c
	$(CC) $(INCLUDE) $(FLAGS) test_crash.c -o test_crash $(LIB_S)

test_crash_multi: test_crash_multi.c
	$(CC) $(INCLUDE) $(FLAGS) test_crash_multi.c -o test_crash_multi $(LIB_S)

test_recovery_ext4: test_recovery_ext4.c
	$(CC) $(INCLUDE) $(FLAGS) test_recovery_ext4.c ../crc32/crc32.o -o test_recovery_ext4 -lpthread -lrt

crfs_exit: crfs_exit.c
	$(CC) $(FLAGS) crfs_exit.c -o crfs_exit $(LIB_S)

clean:
	rm -rf *.o
	rm -rf *.so
	rm -rf *.so
	rm -rf test_micro test_micro_offload test_micro_posix_compound test_micro_posix test_openwriteclose_offload test_openwriteclose_posix test_scheduler test_encryption test_encryption_posix test_encryption_crfs test_crash test_crash_multi test_recovery_ext4 crfs_exit

