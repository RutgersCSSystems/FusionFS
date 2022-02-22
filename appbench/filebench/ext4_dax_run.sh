#!/bin/bash

FSPATH=/mnt/pmemdir

APP="filebench"
RESULTDIR=$RESULTS/$APP/"result-ext4dax"

set -x
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

OUTPUT=$RESULTDIR/vamail.txt
sudo ./filebench -f $FILEBENCH/myworkloads/varmail_ext4.f &> $OUTPUT
cat $OUTPUT
exit

set +x
