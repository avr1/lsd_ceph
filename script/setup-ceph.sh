#! /bin/sh

sudo apt-get update && sudo apt-get install git cmake

git clone https://github.com/ceph/ceph --depth 1

cd ceph

./run-make-check.sh

cmake --build build

cd build

env MON=1 MDS=1 ../src/vstart.sh -d -n -x






