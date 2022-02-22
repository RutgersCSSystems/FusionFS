#! /bin/bash
set -x

PARAFS=$OFFLOADBASE
DBPATH=/mnt/ram
FSTYPE="crossfs"
APP="encryption"
RESULTDIR=$RESULTS/$APP/"result-crossfs"

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
	valusesize=$2
	threads=$1
	OUTPUT="crossfs_encryption_"$1"_"$2".txt"
        ./test_encryption $valusesize $threads &> $RESULTDIR/$OUTPUT
        cat $RESULTDIR/$OUTPUT
	sleep 2
}

declare -a sizearr=("256" "512" "1024" "4096")
declare -a threadarr=("1")

for valuesize in "${sizearr[@]}"
do
	for thrd in "${threadarr[@]}"
	do
	        CLEAN
		FlushDisk
		RUN $thrd $valuesize
	done
done

unset sizearr
unset threadarr

declare -a sizearr=("4096")
declare -a threadarr=("4" "8" "16")

for valuesize in "${sizearr[@]}"
do
	for thrd in "${threadarr[@]}"
	do
	        CLEAN
		FlushDisk
		RUN $thrd $valuesize
	done
done

