#!/bin/bash

if [ "$#" -ne 3 ]; then
    echo "Usage: sudo ./run_fs.sh <LoadA/RunA/RunB/RunC/RunF/RunD> <fs> <run_id>"
    exit 1
fi

set -x

workload=$1
fs=$2
run_id=$3
cur_dir=`readlink -f ./`
src_dir=`readlink -f ../`
pmem_dir=/mnt/pmem_emul
leveldb_dir=$src_dir
leveldb_build_dir=${leveldb_dir}
database_dir=$pmem_dir/leveldbtest-1000
workload_dir=$leveldb_dir/workloads
result_dir=$cur_dir/results
fs_results=$result_dir/$fs/$workload

ulimit -c unlimited

echo Sleeping for 5 seconds . . 
sleep 5

load_workload()
{
    tracefile=$1

    echo ----------------------- LevelDB YCSB Load $workloadName ---------------------------

    # Clear the database directory
    rm -rf $database_dir

    export trace_file=$tracefile
    echo Trace file is $trace_file

    mkdir -p $fs_results
    rm $fs_results/run$run_id

    date

    $leveldb_build_dir/db_bench --use_existing_db=0 --benchmarks=ycsb --db=$database_dir --threads=1 --open_files=1000 2>&1 | tee $fs_results/run$run_id

    date

    rm $pmem_dir/*

    echo Sleeping for 5 seconds . .
    sleep 5
}

run_workload()
{
    tracefile=$1

    echo ----------------------- LevelDB YCSB Run $workloadName ---------------------------

    export trace_file=$tracefile
    echo Trace file is $trace_file

    mkdir -p $fs_results
    rm $fs_results/run$run_id

    date
    
    $leveldb_build_dir/db_bench --use_existing_db=1 --benchmarks=ycsb --db=$database_dir --threads=1 --open_files=1000 2>&1 | tee $fs_results/run$run_id

    date

    rm $pmem_dir/*

    echo Sleeping for 5 seconds . .
    sleep 5

}


case "$workload" in
    LoadA)
        trace_file=$workload_dir/loada_5M
        load_workload $trace_file
        ;;

    RunA)
        trace_file=$workload_dir/runa_5M_5M
        run_workload $trace_file
        ;;

    RunB)
        trace_file=$workload_dir/runb_5M_5M
        run_workload $trace_file
        ;;

    RunC)
        trace_file=$workload_dir/runc_5M_5M
        run_workload $trace_file
        ;;

    RunF)
        trace_file=$workload_dir/runf_5M_5M
        run_workload $trace_file
        ;;

    RunD)
        trace_file=$workload_dir/rund_5M_5M
        run_workload $trace_file
        ;;

    LoadE)
        trace_file=$workload_dir/loade_5M
        load_workload $trace_file
        ;;

    RunE)
        trace_file=$workload_dir/rune_5M_1M
        run_workload $trace_file
        ;;
    *)
        echo $"Usage: sudo $0 {LoadA/RunA/RunB/RunC/RunF/RunD} {run_id}"
        exit 1
esac

cd $cur_dir
