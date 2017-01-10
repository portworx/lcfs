# Installing Px-Graph on docker 1.13+

To install Px-Graph, there are four actions you must perform:

1. Install LCFS onto your system at `/var/lib/docker` and `/lcfs`.
2. Start Docker using VFS as a graph driver.  This is needed to install Px-Graph as a graph driver in Docker's configuration files at `/var/lib/docker`.
3. Install the Px-Graph plugin.
4. Now you can restart Docker to use Px-Graph

These four steps are detailed below.

##  Step 1 - Install LCFS
1. git clone the repo `git@github.com:portworx/px-graph.git`
2. Build lcfs following the instructions in that [directory](https://github.com/portworx/px-graph/blob/master/lcfs/README.md).
3. Stop docker - for example, `sudo systemctl stop docker`
4. Chose a device to provide to lcfs.  lcfs requires a block device (you can also use a file, but this is not recommended due to performance reasons).  In this example, we use `/dev/sdb`.
5. Remove `/var/lib/docker` and `/lcfs` if they are present.
6. Start lcfs
```
# sudo rm -fr /var/lib/docker /lcfs
# sudo mkdir /lcfs /var/lib/docker
# sudo ./lcfs /dev/sdb /var/lib/docker /lcfs
```

## Step 2 - Start Docker using VFS
Restart the Docker daemon and instruct it to use vfs as the graph driver.  We will restart docker to use lcfs after in step #4.
```
# sudo dockerd -s vfs
```

## Step 3 - Install Px-Graph
Install the Px-Graph driver using the instructions in that [directory](https://github.com/portworx/px-graph/tree/master/plugin/README.md).

## Step 4 - Restart Docker to use Px-Graph
Restart Docker to use Px-Graph with lcfs.  First stop dockerd.  Then run Docker as:
```
# sudo dockerd -s portworx/px-graph
```

Verify docker is running with lcfs by checking the output of command 'docker info'.


> Note: Once Px-Graph has been installed on your system, you do not need to redo these steps even across Docker restarts and system reboots.
