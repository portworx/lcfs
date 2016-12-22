# Building px-graph with fuse v3.0
## WARNING: Docker build is broken with fuse v3.0

This file system driver is implemented using fuse low level API.

1. Download this directory

    ```
    git clone git@github.com:portworx/px-graph
    ```

2. Install fuse 3.0 library.

   a.  Download fuse library from the following link:

   *  https://github.com/libfuse/libfuse/releases/download/fuse-3.0.0/fuse-3.0.0.tar.gz

   b.  Install tools to build fuse:

   * **Centos:** 
     `yum install gcc libstdc++-devel gcc-c++ curl-devel libxml2-devel openssl-devel mailcap`

   * **Ubuntu:**
     `apt-get install build-essential libcurl4-openssl-dev libxml2-dev mime-support`

3. Untar it and build/install using following commands:

    ```
    ./configure
    make -j8
    make install
    ```

   If needed, export PKG_CONFIG_PATH with /usr/local/lib/pkgconfig. Configure LD_LIBRARY_PATH if necessary.

4. Install tcmalloc or remove that from Makefile.

    On Ubuntu, run 

    ```
    sudo apt-get install libgoogle-perftools-dev
    ```

    On CentOS, run

    ```
    sudo yum install gperftools
    ```

5. Build lcfs directory by running make. (cd px-graph/lcfs; make)

6. Mount a device/file - "sudo ./lcfs 'device' 'mnt'". Check output of mount
   command to make sure device is mounted correctly.  It is recommended to use
   an empty directory as mount point.

    For debugging, options -f or -d could be specified.

7. Build px-graph/plugin/lcfs_plugin.go after installing the necessary go packages.

    Set up GOPATH and run the following commands.

    ```
    go get github.com/Sirupsen/logrus github.com/docker/docker/daemon/graphdriver github.com/docker/docker/pkg/archive github.com/docker/docker/reexec github.com/docker/go-plugins-helpers/graphdriver

    cd ../px-graph/plugin
    go build -o lcfs_plugin lcfs_plugin.go
    sudo ./lcfs_plugin
    ```

8. Stop docker and start docker with arguments "-s lcfs -g 'mnt'".

    `-g` argument is needed only if 'mnt' is not /var/lib/docker.

    These options could be configured in file /etc/default/docker or specified as arguments like "sudo dockerd -g 'mnt' -s lcfs".

9. Run experiments, stop docker, umount - "sudo fusermount -u 'mnt'"

10. For displaying stats, run "cstat 'id' [-c]" from 'mnt'/lcfs directory.

   Make sure fuse mount is running in forground mode (-d/-f option).

   Normally, stats are displayed whenever a layer is unmounted.
