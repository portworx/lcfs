#!/bin/bash
set -x

make clean
make
sudo rm -fr rootfs 2>/dev/null

docker image inspect rootfsimage
if [ $? -eq 0 ]; then
    docker rmi rootfsimage
fi

docker build -t rootfsimage .
id=$(docker create rootfsimage)
mkdir rootfs
docker export "$id" | tar -x -C rootfs/

docker plugin inspect portworx/lcfs
if [ $? -eq 0 ]; then
    docker plugin disable portworx/lcfs
    docker plugin rm portworx/lcfs
fi
sudo docker plugin create portworx/lcfs .
docker plugin enable portworx/lcfs

sudo rm -fr rootfs/
docker rm -vf "$id"
docker rmi rootfsimage

docker plugin ls
