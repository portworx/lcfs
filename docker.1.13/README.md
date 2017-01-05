*   Build lcfs following instructions in that directory.

*   Stop docker if that is running - sudo service docker stop

*   Mount a device as lcfs file system at /var/lib/docker and /lcfs.
    It is recommended to remove those directories if present.

    ```
    sudo rm -fr /var/lib/docker /lcfs
    sudo mkdir /lcfs /var/lib/docker
    sudo ./lcfs 'device' /var/lib/docker /lcfs -f
    ```

    When the above process exits, lcfs is unmounted as well.

*   Make sure docker 1.13 is installed on the system (docker version)
    Restart docker with "-s vfs" argument.

*   Create lcfs plugin by following instructions in plugin directory.

    ```
    Set up GOPATH
    # go get github.com/Sirupsen/logrus github.com/docker/docker/daemon/graphdriver github.com/docker/docker/pkg/archive github.com/docker/docker/pkg/reexec github.com/docker/go-plugins-helpers/graphdriver
    #./setup
    ```

*   Stop docker (sudo service docker stop) and restart it with
    arguments '--experimental -s lcfs'

*   Verify docker is running as expected with lcfs graphdriver by
    checking the output of command 'docker info'.

For restarting, stop docker, unmount /lcfs and mount
lcfs again at /var/lib/docker and /lcfs and restart docker with lcfs
graphdriver.  No need to re-create lcfs plugin.

If lcfs needs to recreated, zero out first block (4KB) of the device
when the file system is unmounted.
