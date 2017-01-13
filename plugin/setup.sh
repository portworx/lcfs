#!/bin/bash -xe
set -x

make clean
make
sudo rm -fr rootfs 2>/dev/null

docker build -t rootfsimage .
id=$(docker create rootfsimage)
mkdir rootfs
docker export "$id" | tar -x -C rootfs/
sudo docker plugin create portworx/px-graph .
docker plugin enable portworx/px-graph

sudo rm -fr rootfs/
docker rm -vf "$id"
docker rmi rootfsimage

docker plugin ls
