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
# apk update && apk add bash util-linux qemu qemu-img
# curl -fsSL http://lcfs.portworx.com/latest-alpine/lcfs-setup.sh | LCFS_PKG=http://lcfs.portworx.com/latest-alpine/lcfs-alpine.binaries.tgz DEV=/dev/nbd0 bash
```
As of now, this step is required everytime Docker is restarted from the Menu.  Docker may be manually restarted from the VM by running "/etc/init.d/lcfs restart".

## Uninstalling LCFS
To uninstall the LCFS, simply restart Docker from the Menu.  When Docker is up, remove the device image used for lcfs.

```
# /opt/pwx/bin/lcfs-setup.sh --remove
```

Note that your original image data from the previous driver will be intact.
