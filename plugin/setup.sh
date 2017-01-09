#!/bin/bash -xe
set -x

make clean
make
sudo rm -fr rootfs 2>/dev/null 2>/dev/null

docker build -t rootfsimage .
id=$(docker create rootfsimage)
mkdir rootfs
docker export "$id" | tar -x -C rootfs/
sudo docker plugin create lcfs .
docker plugin enable lcfs

sudo rm -fr rootfs/
docker rm -vf "$id"
docker rmi rootfsimage

docker plugin ls
