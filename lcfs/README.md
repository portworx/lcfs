# Instructions on building the LCFS file syste with fuse v3.0.0
The LCFS file system depends on fuse v3.0.0.  These instructions walk you through installing fuse and building and testing lcfs as regular filesystem, independent of Docker.

### Install pre-requisite packages
Building this file system requires `tcmalloc`, `zlib` and `fuse`.

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
# sudo apt-get install build-essential libcurl4-openssl-dev libxml2-dev mime-support
```

On CentOS, run

```
# sudo yum install gcc libstdc++-devel gcc-c++ curl-devel libxml2-devel openssl-devel mailcap
```
     
Now dowload and install the fuse library from https://github.com/libfuse/libfuse/releases/download/fuse-3.0.0/fuse-3.0.0.tar.gz

Extract the fuse tarball and build and install fuse using the following commands:

```
# ./configure
# make && make install
```

### Build and install the LCFS file system

Now that the pre-requisite packages are installed, we can build and install lcfs.

```
# git clone git@github.com:portworx/lcfs.git
# export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
```

### Build the lcfs file system
You can now build the lcfs file system by running make in the lcfs/lcfs directory.

```
# cd lcfs/lcfs
# make
```

### Install the lcfs binary
Install lcfs at /usr/sbin

```
# sudo install ./lcfs /usr/sbin
```

At this point, LCFS is available for use by the LCFS graph driver plugin.  You can return to the [plugin install instructions](https://github.com/portworx/lcfs/blob/master/INSTALL.md#step-1---install-lcfs), or test LCFS locally on your system by following the instructions below.

### Test lcfs (Optional)
LCFS stores its data on a phsyical device, for example `/dev/sdb`.  It then deploys two mount points to the host.  The first mount point is used for `/var/lib/docker` and the second mount point is used for storing the layers of a container.  Therefore, when starting LCFS directly, you provide 3 parameters:

1. A device to use
2. The first mountpoint (which is used for `/var/lib/docker`.
3. The third mountpoint (which is used for storing container layers).

You can start lcfs as follows:

```
# cd lcfs/lcfs
# mkdir -p /mnt1 /mnt2
# sudo lcfs daemon /dev/sdb /mnt1 /mnt2
# mount
```

Check the output of the `mount` command to make sure that the device is mounted correctly.  It is recommended to use empty directories as the mount points.

> Note: For debugging, the -d option can be specified.

Now you can use `/mnt1` and `/mnt2` as regular file systems to test that lcfs is functioning correctly.

To unmount lcfs, run:
```
# sudo fusermount -u /mnt2
# sudo fusermount -u /mnt1
```

To display lcfs stats, run "lcfs stats /lcfs 'id' [-c]".  Create /lcfs/lcfs directory if that does not exist.  'id' is the name of the layer.  Specifying '.' as id will display stats for all layers.  If -c is specified, existing stats will be cleared.  Normally, stats are displayed whenever a layer is deleted/unmounted.  

By default, syncer attempts to create checkpoint of the file system every minute.  This could be changed by running the command "lcfs syncer /lcfs time".

By default, lcfs page cache is limited to around 512MB.  This could be changed by running the command "lcfs pcache /lcfs memory".

For recreating the file system, unmount it and zero out the first block (4KB) of the device/file and remount the device/file.
