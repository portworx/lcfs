#!/bin/bash

SUDO=sudo
# Setup archive directories
BASE_DIR=../
PLUGIN_BASE_DIR=$BASE_DIR/plugin/artifacts

if [ -z "${DOCKER_HUB_REPO}" ]; then
    echo -e "Please set DOCKER_HUB_REPO env variable."
    exit -1;
fi

if [ -z "${DOCKER_HUB_LCFS_PLUGIN}" ]; then
    echo -e "Please set DOCKER_HUB_LCFS_PLUGIN env variable."
    exit -1;
fi

if [ -z "${DOCKER_HUB_LCFS_TAG}" ]; then
    echo -e "Please set DOCKER_HUB_LCFS_TAG env variable."
    exit -1;
fi

mkdir -p $PLUGIN_BASE_DIR
cp config.json $PLUGIN_BASE_DIR/
mkdir -p $PLUGIN_BASE_DIR/rootfs

# Copy the lcfs plugin binary
cp $BASE_DIR/lcfs_plugin.bin lcfs_plugin

# Build the base rootfs image
$SUDO docker build -t rootfsimage .
# Create a container from the rootfs image
id=$($SUDO docker create rootfsimage)
# Export and untar the container into a rootfs directory
$SUDO docker export "$id" | tar -x -C $PLUGIN_BASE_DIR/rootfs
# Create a docker v2 plugin
$SUDO docker plugin create $DOCKER_HUB_REPO/$DOCKER_HUB_LCFS_PLUGIN:$DOCKER_HUB_LCFS_TAG $PLUGIN_BASE_DIR
# Remove the temporary container
$SUDO docker rm -vf "$id"
$SUDO docker rmi rootfsimage

# Remove the archive direcgtory
$SUDO rm -rf $PLUGIN_BASE_DIR

