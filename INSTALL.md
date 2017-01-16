# Installing LCFS on docker 1.13+

To install LCFS, there are four actions you must perform:

1. Install LCFS onto your system at `/var/lib/docker` and `/lcfs`.
2. Start Docker using VFS as a graph driver.  This is needed to install the LCFS plugin as a graph driver in Docker's configuration files at `/var/lib/docker`.
3. Install the LCFS plugin.
4. Now you can restart Docker to use the LCFS plugin.

These four steps are detailed below.

##  Step 1 - Install LCFS
1. Build and install lcfs following the instructions in that [directory](https://github.com/portworx/px-graph/blob/master/lcfs/README.md).
2. Stop docker - for example, `sudo systemctl stop docker`
3. Chose a device to provide to lcfs.  lcfs requires a block device (you can also use a file, but this is not recommended due to performance reasons).  In this example, we use `/dev/sdb`.
4. Remove `/var/lib/docker` and `/lcfs` if they are present.
5. Start lcfs
```
# sudo rm -fr /var/lib/docker /lcfs
# sudo mkdir -p /lcfs /var/lib/docker
# sudo ./lcfs /dev/sdb /var/lib/docker /lcfs >/dev/null &
```

## Step 2 - Start Docker using VFS
Restart the Docker daemon and instruct it to use vfs as the graph driver.  We will restart docker to use lcfs after in step #4.
```
# sudo dockerd -s vfs
```

## Step 3 - Install the LCFS plugin
```
# docker plugin install --grant-all-permissions portworx/lcfs
# docker plugin ls
```

Make sure plugin is installed and enabled.

## Step 4 - Restart Docker to use LCFS
Restart Docker to use LCFS.  First stop dockerd.  Then run Docker as:
```
# sudo dockerd -s portworx/lcfs
```

Verify docker is running with portworx/lcfs storage driver by checking the output of command 'docker info'.
