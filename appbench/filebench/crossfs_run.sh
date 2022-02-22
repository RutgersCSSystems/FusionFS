#!/bin/bash

FSPATH=/mnt/ram
APP="filebench"
RESULTDIR=$RESULTS/$APP/"result-crossfs"

set -x
echo $RESULTDIR

# Create output directories
if [ ! -d "$RESULTDIR"  ]; then
        mkdir -p $RESULTDIR
fi

CLEAN() {
        set +x
        rm -rf $FSPATH/*
        set -x
        echo "remove files"

}

FlushDisk() {
	sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
	sudo sh -c "sync"
	sudo sh -c "sync"
	sudo sh -c "echo 3 > /proc/sys/vm/drop_caches"
}

CLEAN
FlushDisk
echo 0 | sudo tee /proc/sys/kernel/randomize_va_space

export PARAFS=parafs
export DEVCORECNT=4
export SCHEDPOLICY=0

OUTPUT=$RESULTDIR/vamail.txt
./filebench -f myworkloads/varmail_crossfs.f &> $OUTPUT
cat $OUTPUT
exit


unset PARAFS

set +x
