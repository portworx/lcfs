# Instructions on building LCFS with fuse v2.9.7
The lcfs file system depends on fuse v2.9.7.  These instructions walk you through installing fuse and building and testing lcfs as regular filesystem, independent of Docker.

### Git clone Px-Graph

```
# git clone git@github.com:portworx/px-graph.git
```

### Install fuse
Install tools to build fuse:

   * **Centos:** 
     `yum install gcc libstdc++-devel gcc-c++ curl-devel libxml2-devel openssl-devel mailcap`

   * **Ubuntu:**
     `apt-get install build-essential libcurl4-openssl-dev libxml2-dev mime-support`
     
Now dowload and install the fuse library from https://github.com/libfuse/libfuse/releases/download/fuse-2.9.7/fuse-2.9.7.tar.gz

> Note: If testing with fuse 3.0 library, download fuse library from https://github.com/libfuse/libfuse/releases/download/fuse-3.0.0/fuse-3.0.0.tar.gz

If needed, export PKG_CONFIG_PATH and LD_LIBRARY_PATH.  If the binaries are built inside the official GO container, this won't be necessary.

```
# export PKG_CONFIG_PATH-/usr/local/lib/pkgconfig
```

Now build and install fuse using the following commands:

```
# ./configure
# make -j8
# make install
```

### Install tcmalloc

On Ubuntu, run 

```
# sudo apt-get install -y libgoogle-perftools-dev
```

On CentOS, run

```
# sudo yum install gperftools
```

### Install zlib

On Ubuntu, run

```
# sudo apt-get install -y zlib1g-dev
```

On CentOS, run

```
# sudo yum install zlib-devel
```

### Build lcfs 
Now you can build lcfs by running make in the px-graph/lcfs directory.

```
# cd px-graph/lcfs
# make
```


### Test lcfs
Choose a device or file to start lcfs with.  For example, `/dev/sdb`.  You can start lcfs as follows:

```
# cd px-graph/lcfs
# sudo ./lcfs /dev/sdb /mnt /mnt2 > /dev/null &
# mount
```

Check the output of the `mount` command to make sure that the device is mounted correctly.  It is recommended to use empty directories as the mount points.

> Note: For debugging, the -d option can be specified.

Now you can use `/mnt` as a regular file system to test that lcfs is functioning correctly.

To unmount lcfs, run:
```
# sudo fusermount -u /mnt2
```

To display lcfs stats, run "cstat 'id' [-c]" from the 'mnt'/px-graph directory.  Create px-graph directory if that does not exist.  'id' is the name of the layer.  Specifying '.' as id will display stats for all layers.  If -c is specified, existing stats will be cleared.  Normally, stats are displayed whenever a layer is deleted/unmounted.  

For recreating the file system, unmount it and zero out the first block (4KB) of the device/file and remount the device/file.
