# Instructions on building LCFS with fuse v2.9.7
The lcfs file system depends on fuse v2.9.7.  These instructions walk you through installing fuse and building and testing lcfs as regular filesystem, independent of Docker.

### Install pre-requisite packages
LCFS requires `tcmalloc`, `zlib` and `fuse`.

#### Install tcmalloc

On Ubuntu, run 

```
# sudo apt-get install -y libgoogle-perftools-dev
```

On CentOS, run

```
# sudo yum install gperftools
```

#### Install zlib

On Ubuntu, run

```
# sudo apt-get install -y zlib1g-dev
```

On CentOS, run

```
# sudo yum install zlib-devel
```

#### Install fuse
Install the pre-requisite packages for fuse.

On Ubuntu, run

```
# apt-get install build-essential libcurl4-openssl-dev libxml2-dev mime-support
```

On CentOS, run

```
# yum install gcc libstdc++-devel gcc-c++ curl-devel libxml2-devel openssl-devel mailcap
```
     
Now dowload and install the fuse library from https://github.com/libfuse/libfuse/releases/download/fuse-2.9.7/fuse-2.9.7.tar.gz

> Note: If testing with fuse 3.0 library, download fuse library from https://github.com/libfuse/libfuse/releases/download/fuse-3.0.0/fuse-3.0.0.tar.gz

Extract the fuse tarball and build and install fuse using the following commands:

```
# ./configure
# make && make install
```

### Build and install LCFS

Now that the pre-requisite packages are installed, we can build and install lcfs.

```
# git clone git@github.com:portworx/lcfs.git
# export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
```

### Build lcfs 
Now you can build lcfs by running make in the px-graph/lcfs directory.

```
# cd lcfs/lcfs
# make
```

### Test lcfs
LCFS stores it's data on a phsyical device, for example `/dev/sdb`.  It then deploys two mount points to the host.  The first mount point is used for `/var/lib/docker` and the second mount point is used for storing the layers of a container.  Therefore, when starting LCFS directly, you provide 3 parameters:

1. A device to use
2. The first mountpoint (which is used for `/var/lib/docker`.
3. The third mountpoint (which is used for storing container layers).

You can start lcfs as follows:

```
# cd lcfs/lcfs
# mkdir -p /mnt1 /mnt2
# sudo ./lcfs /dev/sdb /mnt1 /mnt2 > /dev/null &
# mount
```

Check the output of the `mount` command to make sure that the device is mounted correctly.  It is recommended to use empty directories as the mount points.

> Note: For debugging, the -d option can be specified.

Now you can use `/mnt1` and `/mnt2` as regular file systems to test that lcfs is functioning correctly.

To unmount lcfs, run:
```
# sudo fusermount -u /mnt2
```

To display lcfs stats, run "cstat 'id' [-c]" from the 'mnt'/px-graph directory.  Create px-graph directory if that does not exist.  'id' is the name of the layer.  Specifying '.' as id will display stats for all layers.  If -c is specified, existing stats will be cleared.  Normally, stats are displayed whenever a layer is deleted/unmounted.  

For recreating the file system, unmount it and zero out the first block (4KB) of the device/file and remount the device/file.
