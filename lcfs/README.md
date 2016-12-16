# Building px-graph

This file system driver is implemented using fuse low level API.

1. Download this directory

    ```
    git clone git@github.com:portworx/px-graph
    ```

2. Install fuse library.

    Download fuse library from the following link:

    https://github.com/libfuse/libfuse/releases/download/fuse-2.9.7/fuse-2.9.7.tar.gz

    Untar it and build/install using following commands:

    ```
    ./configure
    make -j8
    make install
    ```

    If needed, export PKG_CONFIG_PATH with /usr/local/lib/pkgconfig.

    **OR** alternately, use yum on RedHat/CentOS to download the most current package:

    ```
    yum install -y fuse
    ```

3. Install tcmalloc or remove that from Makefile.

    On Ubuntu, run 

    ```
    sudo apt-get install libgoogle-perftools-dev
    ```

    On CentOS, run

    ```
    sudo yum install gperftools
    ```

4. Build lcfs directory by running make. (cd px-graph/lcfs; make)

5. Mount a device/file - "sudo ./lcfs 'device' 'mnt'".

    For debugging, options -f or -d could be specified.

6. Build px-graph/plugin/lcfs_plugin.go after installing the necessary go packages.

    Set up GOPATH and run the following commands.

    ```
    go get github.com/Sirupsen/logrus github.com/docker/docker/daemon/graphdriver github.com/docker/docker/pkg/archive github.com/docker/docker/exec github.com/docker/go-plugins-helpers/graphdriver

    cd ../px-graph/plugin
    go build -o lcfs_plugin lcfs_plugin.go
    sudo ./lcfs_plugin
    ```

7.  Stop docker and start docker with arguments "-s lcfs -g 'mnt'".

    `-g` argument is needed only if 'mnt' is not /var/lib/docker.

    These options could be configured in file /etc/default/docker or specified as arguments like "sudo dockerd -g 'mnt' -s lcfs".

8.  Run experiments, stop docker, umount - "sudo fusermount -u 'mnt'"

9.  For displaying stats, run "cstat 'id' [-c]" from 'mnt'/lcfs directory.

    Make sure fuse mount is running in forground mode (-d/-f option). Otherwise, stats are displayed whenever a layer is unmounted.
