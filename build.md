## install go

[https://go.dev/doc/install](https://go.dev/doc/install)

or use script

[https://github.com/canha/golang-tools-install-script](https://github.com/canha/golang-tools-install-script)

check using `go version` preferably version 1.17 

## setting up ceph

Official supported CEPH version for Ubuntu 20.04 are Ceph 15 and 16. Ceph 15 software is supported by default Ubuntu archive but Ceph16 needs UCA. See more [https://ubuntu.com/ceph/docs/supported-ceph-versions](https://ubuntu.com/ceph/docs/supported-ceph-versions)

`git clone -b octopus <https://github.com/ceph/ceph.git`>

`cd ceph`

`checkout one release into a new branch to avoid detached head state. : git checkout tags/v15.2.15 -b v15.2.15`

`./install-deps.sh`

`./do_cmake.sh`

`cd build`

 `make -j ${nproc}` 
Note: make alone will use only one CPU thread, this could take a while. use the -j option to use more threads. Something like make -j$(nproc) would be a good start.
`make -j {# of jobs}`: use  $(nproc) or specify number of jobs explicitly (it's should be <= # of cpu cores). Each job takes roughly 2.5 GB of RAM. Make sure to assign enough memories to VM.`

After build complete,
`../src/vstart.sh -d -n`

`env MON=1 MDS=1 ../src/vstart.sh -d -n -x`

`./bin/ceph -s`

ensure that the cluster is active

## Building [librbd.so](http://librbd.so)

clone the repo: 

[https://github.com/canha/golang-tools-install-script](https://github.com/canha/golang-tools-install-script)

Also, clone the repo wih our source modifications: https://github.com/SiddheshRane/librbdbs3

execute [build.sh](https://github.com/SiddheshRane/librbdbs3/build.sh), ensure librbd.so is present in mylibrbd directory

## Setting up testing tools

### qemu related utils

`apt install qemu-utils`

`apt install qemu-io`

### fio

clone the repo: `git clone[https://github.com/axboe/fio.git](https://github.com/axboe/fio.git)`

`./configure --extra-cflags="-I~/ceph/src/include -L~/ceph/build/lib"`

`make`
`sudo make install`

`export CEPH_CONF=~/ceph/build/ceph.conf`
`export LD_LIBRARY_PATH=~/ceph/build/lib/`

follow the rbd section of ceph fio test guide: [https://github.com/ceph/ceph/tree/master/src/test/fio#rbd](https://github.com/ceph/ceph/tree/master/src/test/fio#rbd)

make sure "Rados Block Device engine" is yes after executing configure

`./fio --enghelp=rbd` should not be error

### MinIO

getting minio server binary

`wget [https://dl.min.io/server/minio/release/linux-amd64/minio](https://dl.min.io/server/minio/release/linux-amd64/minio)`
`chmod +x minio`
`./minio server /data`

add a new user using console gui or cli, inputting credentials or you can use default root user credential for BS3 config. 

modify or create

`/etc/bs3/config.toml`

on credentials and ports

a possible config file could be like: (use your own AWS key)

```bash
# Specify the major of the corresponding buse device you want to configure and
# connect to. E.g. 0 if you want to work with /dev/buse0.
major = 0

# Number of user-space daemon threads which is also a maximal number of queues
# storage stack uses. This is limited to the number of CPUs. I.e. minimal value
# is 1 and maximal is number of CPUs. Optimally it should be set to the number
# of CPUs. 0 means optimal value.
threads = 4

# Size of the created block device in GB.
size = 1 #GB

# Block size of created device. 512 or 4096. It is forbidden to change
# block_size on the existent block device. In B.
block_size = 512 #B

# Whether IOs should be scheduled by linux kernel stack.
scheduler = false

# IO queue depth for created block device.
queue_depth = 256

# Use null backend, i.e. just immediately acknowledge reads and writes and drop
# them. Useful for testing raw BUSE performance. Otherwise useless because all
# data are lost.
null = false

# Enable web-based go pprof profiler for performance profiling.
profiler = false

# Profiler port.
profiler_port = 6060

# Configuration related to AWS S3
[s3]
# AWS Access Key
# use your own key
access_key = ""

# AWS Secret Key
# use your own key
secret_key = ""

# Bucket where to store objects.
bucket = "bs1"

# <protocol>://<ip>:<port> of the S3 backend. AWS S3 endpoint is used when empty string.
# sometimes it's port 9000, depend on system
remote = "http://localhost:8000"

# Region to use.
region = "us-east-1"

# Max number of threads to spawn for uploads and downloads.
uploaders = 384
downloaders = 384

# Configuration specific to write path.
[write]
# Semantics of the flush request. True means durable device, i.e. flush request
# gets acknowledge when data are persisted on the backend. False means
# eventually durable, i.e. flush request just a barrier.
durable = false

# Size of the shared memory between kernel and user space for data being
# written. The size is per one thread. In MB.
shared_buffer_size = 32 #MB

# Size of the chunk. Chunk is the smallest piece which can be sent to the user
# space and where all writes are stored. In MB.
chunk_size = 4 #MB

# The whole address space is divided into collision domains. Every collision
# domain has its own counter for writes' sequential numbers. This is useful
# when we don't want to have one shared counter for writes. Instead we split it
# into parts and save the cache coherency protocol traffic. In MB.
collision_chunk_size = 1 #MB

# Configuration specific to read path.
[read]

# Size of the shared memory between kernel and user space for data being read.
# The size is per one thread. In MB.
shared_buffer_size = 32 #MB

# Garbage Collection related configuration
[gc]
# Step when scanning the extent map. In blocks.
step = 1024

# Threshold for live data in the object. Objects under this threshold are
# garbage collected by the "threshold GC" which is trigerred by SIGUSR1. This
# type of GC is heavy on resources and should be planned by the timer for not
# intense times.
live_data = 0.3

# Timeout to wait before any of requests from GC thread will be served by the
# extent map and object manager. In ms.
idle_timeout = 200

# How many seconds to wait before next periodic GC round. This is related to
# "dead GC" cleaning just dead objects. It very light on resources and does not
# contend for the extent map like the "threshold GC".
wait = 600

# Configuration specific to the logger.
[log]
# Minimal level of logged messages. Following levels are provided:
# panic 5, fatal 4, error 3, warn 2, info 1, debug 0, trace -1
level = 1

# Pretty print means nicer log output for human but much slower than non-pretty
# json output.
pretty = true
```

## Testing process

### Prerequisite setup

1. Start CEPH cluster and setup
    1. cd into /ceph/build directory
    2. ../src/vstart.sh -d -n
    3. create osd pools : rbd 
    
    ```bash
    ./ceph osd pool create rbd 100
    ./ceph osd pool application enable rbd rbd
    ./rbd pool init rbd
    ```
    
      d. create block device inside "rbd" pool: qemu_bd, fio_bd 
    
    ```bash
    ./rbd create --size 200 test_fio
    ```
    
2. Start MinIO server
    1. ./minio server ./data
    2. note server API port. E.g. API: http://192.168.1.172:9000
    3. Root user's default name and password are both "minoadmin".
3. Change /etc/bs3/config.toml to use minio server
    1.  remote = "http://localhost:9000" or should be the same as minio API port.
    2. access_key = "minioadmin"
    3. secret_key = "minioadmin"
    

### Testing with qemu-io:

prerequisites: ensure [librbd.so](http://librbd.so) is present in current directory

process:

```jsx
# Launch qemu-io disk excerciser.
$ LD_PRELOAD=./librbd.so qemu-io

# open connection to ceph image 
> open -o driver=raw rbd:rbd/qemu_bd:conf=~/ceph/build/ceph.conf

# Test write at offset 0
> write 0 20M
```

### Testing with fio:

prerequisites: ensure [librbd.so](http://librbd.so) is present in current directory

1. Create job file named "rbd.fio"
    1. Content
    
    ```jsx
    ######################################################################
    # Example test for the RBD engine.
    # 
    # Runs a 4k sequential read write test against a RBD via librbd
    #
    # NOTE: Make sure you have either a RBD named 'fio_bd' or change
    #       the rbdname parameter.
    ######################################################################
    [global]
    #logging
    #write_iops_log=write_iops_log
    #write_bw_log=write_bw_log
    #write_lat_log=write_lat_log
    ioengine=rbd
    clientname=admin
    runtime=5s
    pool=rbd
    rbdname=fio_bd
    rw=readwrite
    bs=4k
    
    [rbd_iodepth32]
    iodepth=32
    ```
    
2. Disable CONFIG_RBD_POLL in FIO manually
    1. Comment out "#define CONFIG_RBD_POLL" in "config-host.h". Recompile FIO by "make" and then "sudo make install".
    2. See (PR https://github.com/axboe/fio/pull/259) for detail on RBD_POLL macro in FIO. 
    3. Disable this because poll events related functions are not implemented in our librbd shared library
3. Run FIO 
    1. "LD_PRELOAD=./librbd.so ./fio ./rbd.fio"
