#! /bin/bash
set -x

PARAFS=$OFFLOADBASE
DBPATH=/mnt/ram
BENCHMARK=fillrandom,readrandom
WORKLOADDESCR="fillrandom-readrandom"

RESULTDIR=result-crossfsfs

# Create output directories
if [ ! -d "$RESULTDIR" ]; then
	mkdir $RESULTDIR
fi

CLEAN() {
	rm -rf /mnt/ram/*
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
	export PARAFSENV=parafs
	export DEVCORECNT=4
        # RR: 0, CFS: 3
	export SCHEDPOLICY=0
	cd $LEVELDB
	LD_PRELOAD=$DEVFSCLIENT/libshim/shim.so $LEVELDB/db_bench --db=$DBPATH --benchmarks=$BENCHMARK --use_existing_db=0 --num=200000 --value_size=$2 --threads=$1  #&> $RESULTDIR/$WORKLOADDESCR"_"$1"_"$2".txt"
	unset PARAFSENV
	sleep 2
}

declare -a sizearr=("512")
declare -a threadarr=("4")
for size in "${sizearr[@]}"
do
	for thrd in "${threadarr[@]}"
	do
	        CLEAN
		FlushDisk
		RUN $thrd $size
	done
done
