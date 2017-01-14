#!/bin/bash -x

# A script to configure docker with portworx/px-graph storage driver on Ubuntu

#Default docker directory
MNT=/var/lib/docker

#Default plugin directory.  Do not change this without updating config.json.
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

#Choose a device for lcfs
export DEVICE=/dev/sdb
sudo dd if=/dev/zero of=$DEVICE count=1 bs=4096

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

WDIR=/tmp/lcfs
rm -fr $WDIR
mkdir -p $WDIR
chmod 777 $WDIR
cd $WDIR

#Install FUSE 2.9.7
wget https://github.com/libfuse/libfuse/releases/download/fuse-2.9.7/fuse-2.9.7.tar.gz
tar -xzvf fuse-2.9.7.tar.gz
cd fuse-2.9.7
./configure
make -j8
sudo make install
rm -fr fuse-2.9.7*

cd $WDIR

# Build lcfs
git clone git@github.com:portworx/px-graph
cd px-graph/lcfs
make

#Mount lcfs
sudo mkdir -p $MNT $MNT2
sudo $WDIR/px-graph/lcfs/lcfs $DEVICE $MNT $MNT2 &
sleep 3

#Restart docker
sudo dockerd -s vfs &
sleep 3

#Create and enable Px-Graph plugin
docker plugin install --grant-all-permissions portworx/px-graph
docker plugin ls

#Restart docker with Px-Graph
sudo pkill dockerd
sleep 3
sudo dockerd -s portworx/px-graph &
sleep 3
docker info

rm -fr $WDIR

#pkill dockerd
#fusermount -u $MNT2 $MNT
