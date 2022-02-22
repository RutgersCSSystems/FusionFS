#! /bin/bash
set -x

PARAFS=$OFFLOADBASE
DBPATH=/mnt/pmemdir

APP="microbench"
RESULTDIR=$RESULTS/$APP/"result-ext4"

# Create output directories
if [ ! -d "$RESULTDIR" ]; then
	mkdir -p $RESULTDIR
fi

CLEAN() {
	rm -rf $DBPATH/*
	sudo killall "db_bench"
	sudo killall "db_bench"
	echo "KILLING Rocksdb db_bench"
}

FlushDisk() {
	sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
	sudo sh -c "sync"
	sudo sh -c "sync"
	sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
}

RUN() {
        ./test_micro_posix $2 4096 $1 
	sleep 2
}

declare -a sizearr=("1" "2")
declare -a threadarr=("1" "4" "8" "16")
for size in "${sizearr[@]}"
do
	for thrd in "${threadarr[@]}"
	do
	        CLEAN
		FlushDisk
		RUN $thrd $size
	done
done
