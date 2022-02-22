all: test_crc32 gen_crc32table crc32c_leveldb
CODE=.
CXX=g++
CC=gcc
#LIB_S=-lpthread -lrt -L../ -ldevfslib
FLAGS+=-DLEVELDB_OFFLOAD_CHECKSUM

test_crc32: crc32.c
	$(CC) -fPIC -c crc32.c
	#$(CC) -DUNITTEST -g crc32.c -o test_crc32

gen_crc32table: gen_crc32table.c
	$(CC) $(INCLUDE) gen_crc32table.c -o gen_crc32table #$(LIB_S)

crc32c_leveldb: crc32c_leveldb.c
	$(CC) -g crc32c_leveldb.c -o crc32c_leveldb

clean:
	rm -rf *.o
	rm -rf *.so
	rm -rf *.so
	rm -rf gen_crc32table test_crc32 crc32c_leveldb
	

