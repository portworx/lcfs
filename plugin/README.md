# Build lcfs plugin code

  Set up GOPATH and run the following commands.

    ```
    # go get github.com/Sirupsen/logrus github.com/docker/docker/daemon/graphdriver github.com/docker/docker/pkg/archive github.com/docker/docker/pkg/reexec github.com/docker/go-plugins-helpers/graphdriver

    # cd $GOPATH/github.com/portworx/px-graph/plugin
    # go build -o lcfs_plugin lcfs_plugin.go
    ```

  You can run the plugin manually by

    ```
    # ./lcfs_plugin
    ```

# Create a v2 lcfs graphdriver plugin

  To create a v2 lcfs plugin we require:
  1. rootfs directory which represents the root filesystem of the plugin
  2. config.json file which describes the plugin

  We will create the rootfs directory by building a Docker images
  which has the compiled lcfs_plugin binary. Make sure that dockerd is running.

  Make sure lcfs is mounted at /lcfs

    ```
    # ./setup
    ```

  Now you rootfs/ subdirectory will be populated with the necessary
  files and the lcfs plugin.  Restart docker.

    ```
    # pkill dockerd

    # /usr/bin/dockerd --experimental -s lcfs

    ```

# Issues

1. Using docker v2 plugins GraphDriver.ApplyDiff fails.  Docker is using
   /var/lib/docker on host to download images and the plugin is not able to
   find those.

2. Tested with docker commit ba76c92
   Docker build is broken because VOLUME command hangs.
