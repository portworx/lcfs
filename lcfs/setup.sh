#!/bin/bash -x

#Device to be mounted as lcfs file system
DEVICE=$1

# Default docker directory
MNT=/var/lib/docker

# Default plugin directory.  Do not change this without updating config.json.
MNT2=/lcfs

# Stop docker
#sudo systemctl stop docker
sudo service docker stop
sudo pkill dockerd
sleep 3

# Unmount mount points
sudo fusermount -u $MNT2
sudo fusermount -u $MNT
sudo umount -f $MNT2 $MNT/* $MNT

# Recreate mount points
sudo rm -fr $MNT $MNT2
sudo mkdir -p $MNT $MNT2

# Build lcfs
make clean
make

# Initialize the device for lcfs
sudo dd if=/dev/zero of=$DEVICE count=1 bs=4096

sudo ./lcfs $DEVICE $MNT $MNT2

# sudo fusermount -u $MNT2
# sudo fusermount -u $MNT
