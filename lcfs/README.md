# Instructions on building LCFS with fuse v2.9.7
The lcfs file system depends on fuse v2.9.7 or higher.  These instructions walk you through installing fuse and building and testing lcfs as regular filesystem, independant of Docker.

### Git clone Px-Graph

```
# git clone git@github.com:portworx/px-graph
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

Install tcmalloc or remove that from the Makefile.

On Ubuntu, run 

```
# sudo apt-get install libgoogle-perftools-dev
```

On CentOS, run

```
# sudo yum install gperftools
```

Now build and install fuse using following commands:

```
# ./configure
# make -j8
# make install
```

### Build lcfs 
Now you can build lcfs by running make in the px-graph/lcfs directory.


### Test lcfs
Chose a device or file to start lcfs with.  For example, `/dev/sdb`.  You can start lcfs as follows:

```
# sudo ./lcfs /dev/sdb /mnt /mnt2
# mount
```

Check the output of the `mount` command to make sure device is mounted correctly.  It is recommended to use an empty directory as mount point.

> Note: For debugging, options -f or -d could be specified.

Now you can use `/mnt` as a regular file system to test that lcfs is functioning correctly.

To unmount lcfs, run:
```
# sudo fusermount -u /mnt
```

To display lcfs stats, run lcfs in forground mode (-d/-f option) and run "cstat 'id' [-c]" from the 'mnt'/lcfs directory.
