# Installing px-graph on docker 1.13+

Follow these instructions to build Px-Graph on your host system and configure Docker to use this graph driver.

## First install LCFS
1. Build lcfs following the instructions in that [directory](https://github.com/portworx/px-graph/tree/master/lcfs).
2. Stop docker - for example, `sudo systemctl stop docker`
3. Chose a device to provide to lcfs.  lcfs requires a block device (you can also use a file, but this is not recommended due to performance reasons).  In this example, we use `/dev/sdb`.
4. Remove `/var/lib/docker` and `/lcfs` if they are present.
5. Start lcfs
```
# sudo rm -fr /var/lib/docker /lcfs
# sudo mkdir /lcfs /var/lib/docker
# sudo ./lcfs /dev/sdb /var/lib/docker /lcfs
```
6. Restart the Docker daemon and instruct it to use vfs as the graph driver.  We will restart docker to use lcfs after in a few steps below.
```
# sudo dockerd -s vfs
```

## Next install the Px-Graph plugin
1. Install the Px-Graph driver using the instructions in that [directory](https://github.com/portworx/px-graph/tree/master/plugin).
2. Restart Docker to use Px-Graph with lcfs.  First stop dockerd.  Then run Docker as:
```
# sudo dockerd -s lcfs
```
3. Verify docker is running with lcfs by checking the output of command 'docker info'.


> Note: Once Px-Graph has been installed on your system, you do not need to redo these steps even across Docker restarts and system reboots.
