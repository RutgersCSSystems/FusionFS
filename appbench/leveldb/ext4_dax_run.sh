#! /bin/bash
set -x

PARAFS=$OFFLOADBASE
DBPATH=/mnt/pmemdir
BENCHMARK=fillrandom,readrandom
workload="fillrandom-readrandom"
FSTYPE="ext4dax"
APP="leveldb"
RESULTDIR=$RESULTS/$APP/"result-ext4dax"
KEYCOUNT="100000"

# Create output directories
if [ ! -d "$RESULTDIR" ]; then
	mkdir -p $RESULTDIR
fi

CLEAN() {
	rm -rf /mnt/ram/*
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
	valsize=$2
	thrdcount=$1
	./db_bench --db=$DBPATH --benchmarks=$BENCHMARK --use_existing_db=0 --num=$KEYCOUNT --value_size=$valsize --threads=$thrdcount  #&> $RESULTDIR/$workload"_"$1"_"$2".txt"
	sleep 2
}

#declare -a sizearr=("100" "512" "1024" "4096")
declare -a sizearr=("512" "4096")
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
