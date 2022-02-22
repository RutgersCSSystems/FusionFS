FusionFS
=================================================================

This repository contains the reference implementation of **FusionFS: Fusing I/O Operations using CISC<sub>Ops</sub> in Firmware File Systems, FAST '22**.


### FusionFS directory structure

    ├── libfs                          # userspace library (LibFS)
    ├── libfs/scripts                  # scripts to mount FusionFS and microbenchmark scripts
    ├── libfs/benchmark                # microbenchmark executables
    ├── kernel/linux-4.8.12            # Linux kernel
    ├── kernel/linux-4.8.12/fs/crfs    # emulated device firmware file system (StorageFS)
    ├── appbench                       # application workloads
    ├── LICENSE
    └── README.md
    
### FusionFS Hardware and OS Environment

FusionFS can be run on both emulated NVM and real Optane-based NVM platform by reserving a region of physical memory (similar to DAX), mounting, and use the region for storing filesystem meta-data and data. 

To enable users to use generally available machine, this documentation will mainly focus on emulated (DAX-based) NVM platform. Users can create a cloudlab instance to run our code (see details below). 

We currently support Ubuntu-based 16.04 kernels and all pacakge installation scripts use debian. While our changes would also run in 18.04 based Ubuntu kernel, due recent change in one of packages (Shim), we can no longer confirm this. Please see Shim discussion below.

#### Getting and Using Ubuntu 16.04 kernel
We encourage users to use NSF CloudLab (see for details). Use the image type "UWMadison744-F18" to create a cloudlab instance.


#### CloudLab - Partitioning a SSD and downloading the code.
If you are using FusionFS in CloudLab, the root partition is only 16GB for some profiles.
First setup the CloudLab node with SSD and install all the required libraries.

```
lsblk
```

You should see the following indicating the root partition size is very small:
```
NAME   MAJ:MIN RM   SIZE RO TYPE MOUNTPOINT
sda      8:0    0 447.1G  0 disk 
├─sda1   8:1    0    16G  0 part /
├─sda2   8:2    0     3G  0 part 
├─sda3   8:3    0     3G  0 part [SWAP]
└─sda4   8:4    0 425.1G  0 part 
sdb      8:16   0   1.1T  0 disk 
```

The root partition only has limited space, hence we need to setup a larger partition to store FusionFS code repository.
First, please use the following script to setup available disk space in this cloudlab instance:

```
git clone https://github.com/ingerido/CloudlabScripts
cd CloudlabScripts
./cloudlab_setup.sh
```
Since `/dev/sda4` is already partitioned, you could just press "q" in fdisk. Otherwise, if you are using another parition, please use the following steps.
Type 'n' and press enter for all other prompts
```
Command (m for help): n
Partition type
   p   primary (0 primary, 0 extended, 4 free)
   e   extended (container for logical partitions)
Select (default p):
....
Last sector, +sectors or +size{K,M,G,T,P} (2048-937703087, default 937703087):
....
Created a new partition 1 of type 'Linux' and of size 447.1 GiB.
```

Now, time to save the partition. When prompted, enter 'w'. Your changes will be persisted.
```
Command (m for help): w
The partition table has been altered.
Calling ioctl() to re-read partition table.
Syncing disks.
.....
```
This will be followed by a script that installs all the required libraries. Please wait patiently 
to complete. Ath the end of it, you will see a mounted SSD partition.


And then after finish, type:

```
findmnt
```

You will see:

```
/users/$Your_User_Name/ssd                 /dev/sda4   ext4        rw,relatime,data=ordered
```

When compiling our Linux kernel, you will need to reboot the machine. Hence we suggest you also modify /etc/fstab to make sure the ssd partition will be mounted automatically during system boot.

```
sudo vim /etc/fstab

/dev/sda4       /users/$Your_User_Name/ssd     ext4    defaults        0       0
```

### Compiling FusionFS

#### Get FusionFS source code on Github
```
cd ~/ssd
git clone https://github.com/RutgersCSSystems/FusionFS-final
```

#### Install required libraries for FusionFS
```
cd FusionFS-final 
./cloudlab.sh
```
NOTE: If you are prompted during Ubuntu package installation, please hit enter and all the package installation to complete.

#### Setting environmental variables
```
source scripts/setvars.sh
```

#### Setup for emulated NVM machine in CloudLab

#### Compile kernel and install

First compile the FusionFS kernel with emulated StorageFS. Please note that in the following benchmarks, demos and applications, the FusionFS kernel need to be modified with different flags and recompiled due to some engineering complexities during implementation.

```
$BASE/scripts/compile_kernel_install.sh (Only run this when first time install kernel)
$BASE/scripts/compile_kernel.sh (Run this when kernel is already installed)
```

Now, time to update the grub reserve NVM in the emulated region.

```
sudo vim /etc/default/grub
```

Add the following to GRUB_CMDLINE_LINUX variable in /etc/default/grub
```
GRUB_CMDLINE_LINUX="memmap=80G\$60G"
```
This will reserve contiguous 60GB memory starting from 80GB. This will ideally
use one socket (numa node) of the system. If your system has a lower memory
capacity, please change the values accordingly.

Update grub and then reboot.
```
sudo update-grub
sudo reboot
```

After the reboot, the reserved region of memory would disappear from the OS.

### Compiling Userspace Libraries

#### Build and install user-space library (LibFS) and required libraries:
```
cd ssd/FusionFS-final
source scripts/setvars.sh
cd $LIBFS
source scripts/setvars.sh
make && make install

cd kcapi
make
make install
cd ..
```

Next, time to build SHIM library derived and enahnced from Strata [SOSP '17]. (**Filebench and Microbenchmark do not need to use shim library**)
This library is responsible for intercepting POSIX I/O operations and directing them to the FusionFS client 
library for converting them to FusionFS commands.

Please note that SHIM library is currently working in 16.04 and is not support 18.04 kernels due to their recent update 
(https://github.com/pmem/syscall_intercept/issues/106). We are in the process of fixing it by ourself.

```
cd $LIBFS/libshim
./makeshim.sh
cd ..
```

#### Mounting FusionFS

```
cd $BASE/libfs
source scripts/setvars.sh
./scripts/mount_fusionfs.sh //(Mount FusionFS with 60GB (Starting at 80GB))
```
If successful, you will find a device-level FS mounted as follows after executing the following command
```
mount
```
none on /mnt/ram type crfs (rw,relatime,...)

NOTE: This is a complex architecture with threads in host and kernel-emulated device. If something hangs, 
try running the following and killing the process and restarting. We are doing more testing and development.

```
cd $LIBFS
benchmark/crfs_exit
```

## Running Microbenchmarks

Assume FusionFS is mounted and current directory is the project root directory.

#### Run micro-bench

To run Microbenchmark for Open-Write-Close.

```
cd libfs/benchmark/
make
# Open-Write-Close, example: ./test_openwriteclose_offload 1000 2 4
./test_openwriteclose_offload file_num file_size(MB) thread_count
```

To run Microbenchmark for Append-CRC-Write and Read-Modify-Write.

```
./test_micro_offload benchmark_type(1 or 2) IO_size thread_count <file_size (MB)>

# Example: ./test_micro_offload 1 4096 4
Benchmark 1: Append-CRC-Write
Benchmark 2: Read-modify-write
```

#### Running Device Compute Fairnes

First, we need to make sure CFS scheduler is enabled. So we need to check the following two flags. 

Enable `_CFS_SCHED` in the kernel file: `kernel/linux-4.8.12/include/linux/devfs.h`

`DEFAULT_SCHEDULER_POLICY` equals to `3` (means using CFS) in libfs file: `libfs/fusionfslib.h`

These two flags are enabled by default, otherwise you need to recompile the kernel and libfs.

To run the Device Compute Fairnes test

```
cd libfs/benchmark/
make
# Example: ./test_scheduler 1 4096 4 4
./test_scheduler benchmark_type IO_size io_thread_count compute_thread_count
Benchmark 1: test CPU fairness
```

If you need to use round robin, `_CFS_SCHED` need to be turn off and change `DEFAULT_SCHEDULER_POLICY` to `0` (means using RR).

## Running Filebench

First, we need to enable a kernel flag to change some configurations for StorageFS.

Enable `CRFS_FILEBENCH` in the kernel file: `kernel/linux-4.8.12/include/linux/devfs.h`

Then recompile the FusionFS kernel

```
./scripts/compile_kernel.sh
sudo reboot
```

After reboot the machine, mount FusionFS (see section Mounting FusionFS), and then build Filebench

```
cd appbench/filebench
./build_filebench.sh
```

Run Filebench with FusionFS

```
./run_fusionfs.sh
```

When finally you finish running Filebench, please disable the kernel flag `CRFS_FILEBENCH` in the kernel file: `kernel/linux-4.8.12/include/linux/devfs.h`. Then recompile the FusionFS kernel.


## Running LevelDB

First, we need to enable a flag `LEVELDB_OFFLOAD_CHECKSUM` in the kernel file: `kernel/linux-4.8.12/include/linux/devfs.h`

Then recompile the FusionFS kernel

```
./scripts/compile_kernel.sh
sudo reboot
```

After reboot the machine, mount FusionFS (see section Mounting FusionFS), and then build and run LevelDB 

```
source scripts/setvars.sh
cd appbench/leveldb/
make
./fusionfs_run.sh
```

When finally you finish running LevelDB, please disable the kernel flag `LEVELDB_OFFLOAD_CHECKSUM` in the kernel file: `kernel/linux-4.8.12/include/linux/devfs.h`. Then recompile the FusionFS kernel to run other tests.

## Running Encryption Offloading

FusionFS also provide encryption offloading, in our *Read-Encrypt-Write* CISCop which read a block, encrypt and write back. The encryption happens in storage hence removes the data copy between the host and storage.

To run the demo, first, make sure the following flag in the kernel file `kernel/linux-4.8.12/include/linux/devfs.h` for micro-transaction is enabled.
```
#define PARAFS_VECTOR_CMD
```
And then compile the kernel and reboot

```
./scripts/compile_kernel.sh
sudo reboot
```

After reboot, mount FusionFS with the following command (Assume current directory is the project root directory.)
```
cd libfs
source scripts/setvars.sh
./scripts/mount_fusionfs.sh
```

Make sure the following flag is enabled in `libfs/Makefile` is enabled.
```
FLAGS+=-D_USE_VECTOR_IO_CMD
```

If the Makefile need to change, please rebuild the LibFS:
```
make clean
make
make install
````

Then go to the benchmark folder and run, example: ./test_encryption <block_size> <thread_nr>
```
cd benchmark
./test_encryption 4096 4
```

## Running recovery test with MacroTx
FusionFS provides MacroTx that views the entire CISCop as an transaction (same crash consistency guarantee with existing POSIX file systems like Ext4, and device-level file systems like CrossFS). However, without fine-grained transaction, the test demo will not able to recover the *Append-Checksum-Write* CISCop when crashed in the middle.

First, make sure the following configuration flag in the kernel file `kernel/linux-4.8.12/include/linux/devfs.h` for micro-transaction is disabled.
```
//#define _MACROFS_JOURN
```
And then compile the kernel and reboot if configuration flags are changed.

```
./scripts/compile_kernel.sh
sudo reboot
```

After reboot, mount FusionFS with the following command (Assume current directory is the project root directory.)
```
cd libfs
source scripts/setvars.sh
./scripts/mount_fusionfs.sh
```

### Running recovery test
In order to emulate the crash scenario, FusionFS uses a "crash injection" approach that manually inject crash point to StorageFS, so that later an I/O operation will be terminated at the injected crash point.

In this demo test, we are testing the *Append-Checksum-Write* CISCop, and inject the crash point when the checksum is put into log entry before persist in place. In MacroTx, the entire CISCop is a transaction, hence any crash happens in the middle will not be recoverable.

The test demo program first performs an 16384 bytes regular append POSIX op, then followed by an *Append-Checksum-Write* CISCop of 16380 bytes (the total append should be 16384 after appending the checksum of 4 bytes).

To run the recovery tests:
```
cd benchmark
./test_crash 7		# 7 is the injected crash code where the crash point is after checksum calculation
```
You are expected to see an error from libFS because we manually injected a crash point. Now check the size of the target file:
```
ll /mnt/ram/testfile
```
It should be 32764 bytes, because the checksum is calculated but without persisted in-place. Now umount FusionFS and remount (as an emulated "reboot from crash"), and then check the file size again
```
cd ..
sudo umount /mnt/ram
./scripts/mount_fusionfs.sh
ll /mnt/ram/testfile
```
Now the file size is still 32764 bytes, because MacroTx does not have the recoverability.


## Running Recovery test with MicroTx and AutoRecovery

AutoRecovery with MicroTx provides fine-grained transaction for CISCop. Here is the example and test demo of using AutoRecovery in an *Append-Checksum-Write* CISCop (illustrated in the section 4.4.2 in the paper).

First, make sure the following configurations flag in the kernel file `kernel/linux-4.8.12/include/linux/devfs.h` are enabled for enabling micro-transaction
```
#define _MACROFS_JOURN
```
And then compile the kernel and reboot
```
./scripts/compile_kernel.sh
sudo reboot
```

After reboot, mount FusionFS with the following command (Assume current directory is the project root directory.)
```
cd libfs
source scripts/setvars.sh
./scripts/mount_fusionfs.sh
```

### Running recovery test
In order to emulate the crash scenario, FusionFS uses a "crash injection" approach that manually inject crash point to StorageFS, so that later an I/O operation will be terminated at the injected crash point.

In this demo test, we are testing the *Append-Checksum-Write* CISCop, and inject the crash point when the checksum is put into log entry before put in place. In our MicroTx with AutoRecovery design, since the second micro-op (checksum calculation) is done, FusionFS is able to recover this *Append-Checksum-Write* CISCop; because (1) the operational log and commit bitmap showing the second micro-op (checksum calculation) is successfuly done, and the next (third) micro-op is an write operation to write the checksum, but not able to process due to crash. (2) The checksum value is persisted in log entry after the second micro-op (checksum calculation) is done. Hence, when re-mounting FusionFS, FusionFS is able to recover from failure and persist the in-pending checksum value in place. (For more details, please see our paper.)

The test demo program first performs an 16384 bytes regular append POSIX op, then followed by an *Append-Checksum-Write* CISCop of 16380 bytes (the total append should be 16384 after appending the checksum of 4 bytes).

To run the recovery tests, the steps are exactly same as running with MacroTx:
```
cd benchmark
./test_crash 7		# 7 is the injected crash code where the crash point is after checksum calculation
```
You are expected to see an error from libFS because we manually injected a crash point. Now check the size of the target file:
```
ll /mnt/ram/testfile
```
It should be 32764 bytes, because the checksum is still in the log-entry, so not yet updated in place. Now umount FusionFS and remount (as an emulated "reboot from crash"), and then check the file size again
```
cd ..
sudo umount /mnt/ram
./scripts/mount_fusionfs.sh
ll /mnt/ram/testfile
```
Now the file size is 32768 bytes after recovery, and if you use hexdump, you should see the checksum value is now recovered.


## Running benchmarks and applications with ext4-DAX
------------------------
If you were running other file systems such as Ext4-DAX, please restart the machine

### Compiling Ext4-DAX

```
cd FusionFS //the base folder
```

#### Setting environmental variables
```
source scripts/setvars.sh
```

#### Setup for emulated NVM machine in CloudLab
```
cd $BASE/scripts/compile_kernel_install.sh (Only run this when first time install kernel)
cd $BASE/scripts/compile_kernel.sh (Run this when kernel is already installed)
```
Now, time to update the grub reserve NVM in the emulated region.

Add "memmap=60G!80G" to /etc/default/grub in GRUB_CMDLIN_LINUX
```
sudo update-grub
sudo reboot
```

#### Mounting Ext4-DAX

```
cd $BASE/libfs
source scripts/setvars.sh
./scripts/mountext4dax.sh
```

### Running Microbenchmarks

```
cd libfs/benchmark/
make
./ext4_dax_run.sh
```

### Running Filebench

```
cd appbench/filebench/
./build_filebench.sh
./ext4_dax_run.sh
```

### Running LevelDB

Make sure the following flags `-DDATA_BLOCK_CHECKSUM_OFFLOAD` and `-DLOG_BLOCK_CHECKSUM_OFFLOAD` are disabled in the build file `build_detect_platform`
```
cd appbench/leveldb/
make -j32
./ext4_dax_run.sh
```

### Running Encryption Test

```
cd libfs/benchmark/
./test_encryption_posix 4096 4
```

## Running benchmarks and applications with CrossFS

See [here](https://github.com/RutgersCSSystems/CrossFS) for how to mount and use CrossFS.

### Running Microbenchmarks

```
cd libfs/benchmark/
make
./crossfs_run.sh
```

### Running Filebench

```
cd appbench/filebench/
./build_filebench.sh
./crossfs_run.sh
```

### Running LevelDB

Make sure the following flags `-DDATA_BLOCK_CHECKSUM_OFFLOAD` and `-DLOG_BLOCK_CHECKSUM_OFFLOAD` are disabled in the build file `build_detect_platform`
```
cd appbench/leveldb/
make -j32
./crossfs_run.sh
```

### Running Encryption Test

```
cd libfs/benchmark/
./test_encryption_crfs 4096 4
```

## Notices
1. StorageFS is currently implemented in the OS kernel as a device driver. Our futrue work will focus on re-implementing and extending our ideas and implementation to the programmable storage devices.
2. Benchmarks and applications are not fully tested in all configurations. Working configurations are described in this README.
3. This software is only for the purpose of research study. There is no warranty; not even for merchantability or fitness for a particular purpose.

