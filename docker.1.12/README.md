*  Build lcfs following instrctions in that directory.

*  Download docker 1.12 sources and add files for recognizing lcfs graphdriver
   copying from this directory.

    ```
    git clone git@github.com:docker/docker
    
    cd docker

    mkdir daemon/graphdriver/lcfs

    cp px-graph/docker.1.12/daemon/graphdriver/lcfs/* daemon/graphdriver/lcfs

    cp px-graph/docker.1.12/daemon/graphdriver/register/lcfs* daemon/graphdriver/register

    git add --all
    
    make build && make binary
    
    sudo service docker stop
    
    sudo cp bundles/latest/binary-client/docker /usr/bin
    
    sudo cp bundles/latest/binary-daemon/dockerd /usr/bin/dockerd
    
    sudo cp bundles/latest/binary-daemon/docker-runc /usr/bin
    
    sudo cp bundles/latest/binary-daemon/docker-containerd /usr/bin
    
    sudo cp bundles/latest/binary-daemon/docker-containerd-ctr /usr/bin
    
    sudo cp bundles/latest/binary-daemon/docker-containerd-shim /usr/bin
    
    ```
*   Mount a device (or file) at /var/lib/docker (this could be different).
    It is recommended to make sure /var/lib/docker is empty before doing this.
    Make sure a lcfs file system is mounted at /var/lib/docker before
    proceeding by running mount command.

*   Restart docker with "-s lcfs" argument.

*   Make sure docker is running with lcfs graphdriver by checking the output of
    'docker info' command.

For restarting from scratch, stop docker, unmount lcfs, zero out first block
(4KB) of the device and repeat above steps.
