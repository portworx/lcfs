Build px-graph/plugin/lcfs_plugin.go after installing the necessary go packages.

    Set up GOPATH and run the following commands.

    ```
    go get github.com/Sirupsen/logrus github.com/docker/docker/daemon/graphdriver github.com/docker/docker/pkg/archive github.com/docker/docker/pkg/reexec github.com/docker/go-plugins-helpers/graphdriver

    go build -o lcfs_plugin lcfs_plugin.go
    sudo ./lcfs_plugin
    ```

Tested with docker commit ba76c92
