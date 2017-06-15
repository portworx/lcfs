# Installing LCFS on Mac

```
Install Docker if needed, following instructions at https://docs.docker.com/docker-for-mac/install 
```
# Make sure Docker VM is configured with at least 4GB of memory
LCFS is running as a Docker V2 plugin and the plugin requires memory for transferring tar archives of images between Docker and storage Driver.  The memory for the Docker VM could be configured from Preferences menu under the Advanced tab.  Docker needs to be restarted for this change to take effect.

## Log into the Docker VM

```
# screen ~/Library/Containers/com.docker.docker/Data/com.docker.driver.amd64-linux/tty 
```

## Install and run LCFS

```
# apk update && apk add bash util-linux qemu qemu-img && curl -fsSL http://lcfs.portworx.com/alpine/lcfs-setup.sh | bash
```
As of now, this step is required everytime Docker is restarted from the Menu.  Docker may be manually restarted from the VM by running "/etc/init.d/lcfs restart".  Docker images and containers will be intact across Docker restart operations if lcfs is stopped before Docker is restarted by issuing the following command.

```
# /etc/init.d/lcfs stop
```

## Growing the backend image (device)
By default, LCFS uses a device /dev/nbd0 which is backed up by /host_docker_app/lcfs-dev.img.  The image is created as a 20GB file when LCFS is installed.  Having a separate device for LCFS keeps the vmdk of the VM from growing as more images and containers are created.  Instead the backend image of the LCFS can be resized as the demand for space grows with more number of images and containers.  That can be done without stopping LCFS or docker.  Here are the steps for doing so, assuming additional 10GB of space is needed.

```
# qemu-nbd -d /dev/nbd0
# qemu-img resize -f raw /host_docker_app/lcfs-dev.img  +10G
# qemu-nbd -f raw -c /dev/nbd0 /host_docker_app/lcfs-dev.img
# /opt/lcfs/bin/lcfs grow /lcfs
```

## Uninstalling LCFS
To uninstall the LCFS, simply restart Docker from the Menu.  When Docker is up, remove the device image used for lcfs.

```
# /opt/lcfs/bin/lcfs-setup.sh --remove
```

Note that your original image data from the previous driver will be intact.
