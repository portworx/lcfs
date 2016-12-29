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

    ```
    # cd $GOPATH/github.com/portworx/px-graph/plugin
    # docker build -t rootfsimage .
    # id=$(docker create rootfsimage)
    # mkdir rootfs
    # docker export "$id" | tar -x -C rootfs/
    ```

  Now you rootfs/ subdirectory will be populated with the necessary
  files and the lcfs plugin.

  Create and Enable the lcfs plugin using the docker plugin commands

    ```
    # ls
      config.json  Dockerfile  lcfs_plugin  lcfs_plugin.go README.md  rootfs

    # docker plugin create lcfs .
      lcfs

    # docker plugin ls
    ID                  NAME                TAG                DESCRIPTION              ENABLED
    c23191e1e161        lcfs                latest             LCFSGraphdriver Plugin   false

    # docker plugin enable lcfs
    lcfs

    # docker plugin ls
    ID                  NAME                TAG                DESCRIPTION              ENABLED
    c23191e1e161        lcfs                latest             LCFSGraphdriver Plugin   true

    # pkill dockerd

    # /usr/bin/dockerd --experimental -s lcfs

    # rm -rf rootfs/
    ```

  Docker expects the graphdriver's base dir to be
  /var/lib/docker/<plugin_name>. So mount the lcfs device on to /var/lib/docker/lcfs


# Issues

1. Using docker v2 plugins GraphDriver.Create fails. Not sure if
   /var/lib/docker/lcfs is the right base directory for lcfs

2. Tested with docker commit ba76c92
   Docker build is broken because VOLUME command hangs.
   Not sure if graphdriver plugin needs to be on top of another graphdriver or
   file system for storing json files.
