#!/bin/bash -x

#Default docker directory
MNT=/var/lib/docker

#Default plugin directory.  Do not change this without updating config.json.
MNT2=/lcfs

#Install docker if needed.

#Stop docker
#systemctl stop docker
service docker stop
pkill dockerd
sleep 3
fusermount -u $MNT2
fusermount -u $MNT
umount -f $MNT2 $MNT
rm -fr $MNT2 $MNT

#Choose a device for lcfs
export DEVICE=/dev/sdb
dd if=/dev/zero of=$DEVICE count=1 bs=4096

export GOPATH=$HOME/portworx
rm -fr $GOPATH
mkdir -p $GOPATH 2>/dev/null

apt-get update

#Install wget
apt-get install -y wget

#Install go
#wget https://storage.googleapis.com/golang/go1.7.4.linux-amd64.tar.gz
#tar -C /usr/local -xzvf go1.7.4.linux-amd64.tar.gz
#rm go1.7.4.linux-amd64.tar.gz
export PATH=$PATH:/usr/local/go/bin

#Install build tools
#apt-get install -y build-essential libcurl4-openssl-dev libxml2-dev mime-support
#yum install gcc libstdc++-devel gcc-c++ curl-devel libxml2-devel openssl-devel mailcap

#Install tcmalloc
apt-get install -y libgoogle-perftools-dev
#yum install gperftools

#Install FUSE 2.9.7
wget https://github.com/libfuse/libfuse/releases/download/fuse-2.9.7/fuse-2.9.7.tar.gz
tar -xzvf fuse-2.9.7.tar.gz
cd fuse-2.9.7
./configure
make -j8
make install
rm -fr fuse-2.9.7*

# Build lcfs
go get -d github.com/portworx/px-graph/...
cd $GOPATH/src/github.com/portworx/px-graph/lcfs
make

#Mount lcfs
mkdir -p $MNT $MNT2
$GOPATH/src/github.com/portworx/px-graph/lcfs/lcfs $DEVICE $MNT $MNT2 &
sleep 3

#Restart docker
dockerd -s vfs &

#Create and enable Px-Graph plugin
cd $GOPATH/src/github.com/portworx/px-graph/plugin
./setup.sh

#Restart docker with Px-Graph
pkill dockerd
sleep 3
dockerd -s portworx/px-graph &
sleep 3
docker info

#pkill dockerd
#fusermount -u $MNT2 $MNT
