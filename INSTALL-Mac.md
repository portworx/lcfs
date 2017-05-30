# Installing LCFS on Mac

## Install Docker if needed, following instructions at https://docs.docker.com/docker-for-mac/install 

## Log into the Docker VM

```
# screen ~/Library/Containers/com.docker.docker/Data/com.docker.driver.amd64-linux/tty 
```

## Install and run LCFS

```
# apk update && apk add bash util-linux qemu qemu-img && qemu-img create -f raw -o size=10G /tmp/lcfs.img && qemu-nbd -f raw -c /dev/nbd0 /tmp/lcfs.img
# curl -fsSL http://lcfs.portworx.com/latest-alpine/lcfs-setup.sh | LCFS_PKG=http://yum.portworx.com/repo/rpms/lcfs/alpine-lcfs-tcmalloc.tgz DEV=/dev/nbd0 bash
```

## Uninstalling LCFS
To uninstall the LCFS, simply restart Docker from the Menu.  When Docker is up, remove the device image used for lcfs.

```
# rm /tmp/lcfs.img
# rm -fr /run/docker/plugins/lcfs.sock
```

Note that your original image data from the previous driver will be intact.
