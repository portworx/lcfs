*   Build lcfs following instructions in that directory.

*   Mount a device as lcfs file system at /lcfs directory.

    ```
    sudo mkdir /lcfs
    sudo ./lcfs 'device' /lcfs -f
    ```

*   Make sure docker 1.13 is running on the system (docker info)
    and create the lcfs plugin by following instructions in
    plugin directory.

    ```
    Set up GOPATH
    # go get github.com/Sirupsen/logrus github.com/docker/docker/daemon/graphdriver github.com/docker/docker/pkg/archive github.com/docker/docker/pkg/reexec github.com/docker/go-plugins-helpers/graphdriver
    #./setup
    ```

*   Stop docker (sudo service docker stop) and restart it with
    arguments '--experimental -s lcfs`

*   Verify docker is running as expected with lcfs graphdriver by
    checking the output of command 'docker info'.

In order to restart from scratch, stop docker, unmount /lcfs and
then zero out the first block (4KB) of the device and 'rm -fr /var/lib/docker'.
Removing /var/lib/docker may fail due to a plugin mount, which can be manually
unmounted.
