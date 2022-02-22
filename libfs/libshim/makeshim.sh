#!/bin/bash

set -x

sudo apt-get -y install pkg-config cmake
#sudo apt-get -y install libcapstone-dev 

if [ ! -d syscall_intercept ]; then
        git clone https://github.com/pmem/syscall_intercept.git
        git clone https://github.com/aquynh/capstone.git
        cd capstone
        ./make.sh
        sudo ./make.sh install
        cd ..
fi

cd syscall_intercept
mkdir build
mkdir install
cd build
cmake -DCMAKE_INSTALL_PREFIX=../install -DCMAKE_BUILD_TYPE=Release ..
make
make install

cd ../../
make

set +x
