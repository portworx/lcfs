#!/bin/bash -x

# A script to configure docker with portworx/lcfs storage driver on Ubuntu

# Device to configure LCFS file system
DEVICE=$1

# Default docker directory
MNT=/var/lib/docker

# Default plugin directory.  Do not change this without updating config.json.
MNT2=/lcfs

#Install docker if needed.

#Stop docker
#sudo systemctl stop docker
sudo service docker stop
sudo pkill dockerd
sleep 3
sudo fusermount -u $MNT2
sudo fusermount -u $MNT
sudo umount -f $MNT2 $MNT
sudo rm -fr $MNT2 $MNT

sudo apt-get update

#Install wget
sudo apt-get install -y wget

#Install build tools
#sudo apt-get install -y build-essential libcurl4-openssl-dev libxml2-dev mime-support
#sudo yum install gcc libstdc++-devel gcc-c++ curl-devel libxml2-devel openssl-devel mailcap

#Install tcmalloc
sudo apt-get install -y libgoogle-perftools-dev
#sudo yum install gperftools

#Install zlib
sudo apt-get install -y zlib1g-dev
#sudo yum install install zlib-devel

#Install urcu
sudo apt-get install -y liburcu-dev

WDIR=/tmp/lcfs
rm -fr $WDIR
mkdir -p $WDIR
chmod 777 $WDIR
cd $WDIR

#Install FUSE 3.0.0
wget https://github.com/libfuse/libfuse/releases/download/fuse-3.0.2/fuse-3.0.2.tar.gz
tar -xzvf fuse-3.0.2.tar.gz
cd fuse-3.0.2
./configure
make -j8
sudo make install
rm -fr fuse-3.0.2*

cd $WDIR

# Build lcfs
git clone git@github.com:portworx/lcfs
cd lcfs/lcfs
make

# Mount lcfs
sudo mkdir -p $MNT $MNT2
sudo $WDIR/lcfs/lcfs/lcfs $DEVICE $MNT $MNT2 -c
sleep 3

# Restart docker
sudo dockerd -s vfs &
sleep 3

# Create and enable lcfs plugin
docker plugin install --grant-all-permissions portworx/lcfs
docker plugin ls

# Restart docker with lcfs
sudo pkill dockerd
sleep 3
sudo dockerd --experimental -s portworx/lcfs &
sleep 3
docker info

rm -fr $WDIR
sudo rm -fr $MNT2/vfs $MNT2/image/vfs

# pkill dockerd
# fusermount -u $MNT2
# fusermount -u $MNT
