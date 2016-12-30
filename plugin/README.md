# Setting up LCFS graphdriver plugin

  Set up GOPATH and run the following commands.

    ```
    # go get github.com/Sirupsen/logrus github.com/docker/docker/daemon/graphdriver github.com/docker/docker/pkg/archive github.com/docker/docker/pkg/reexec github.com/docker/go-plugins-helpers/graphdriver

    ```
# Create a v2 lcfs graphdriver plugin

  Make sure lcfs file system is mounted at /lcfs or modify field source
  in config.json with correct mount point.

    ```
    # cd $GOPATH/github.com/portworx/px-graph/plugin
    # ./setup
    ```

  Restart docker as shown below.

    ```
    # /usr/bin/dockerd --experimental -s lcfs

    ```
# Build lcfs plugin code

    ```
    # cd $GOPATH/github.com/portworx/px-graph/plugin
    # go build -o lcfs_plugin lcfs_plugin.go
    ```

  You can run the plugin manually by

    ```
    # ./lcfs_plugin
    ```
