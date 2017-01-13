#!/bin/bash

# Setup archive directories
BASE_DIR=../
PLUGIN_BASE_DIR=$BASE_DIR/plugin/artifacts

if [ -z "${DOCKER_HUB_REPO}" ]; then
    echo -e "Please set DOCKER_HUB_REPO env variable."
    exit -1;
fi

if [ -z "${DOCKER_HUB_PXGRAPH_PLUGIN}" ]; then
    echo -e "Please set DOCKER_HUB_PXGRAPH_PLUGIN env variable."
    exit -1;
fi

if [ -z "${DOCKER_HUB_PXGRAPH_TAG}" ]; then
    echo -e "Please set DOCKER_HUB_PXGRAPH_TAG env variable."
    exit -1;
fi

mkdir -p $PLUGIN_BASE_DIR
cp config.json $PLUGIN_BASE_DIR/
mkdir -p $PLUGIN_BASE_DIR/rootfs

# Copy the lcfs plugin binary
cp $BASE_DIR/lcfs_plugin.bin lcfs_plugin

# Build the base rootfs image
docker build -t rootfsimage .
# Create a container from the rootfs image
id=$(docker create rootfsimage)
# Export and untar the container into a rootfs directory
docker export "$id" | tar -x -C $PLUGIN_BASE_DIR/rootfs
# Create a docker v2 plugin
docker plugin create $DOCKER_HUB_REPO/$DOCKER_HUB_PXGRAPH_PLUGIN:$DOCKER_HUB_PXGRAPH_TAG $PLUGIN_BASE_DIR
# Remove the temporary container
docker rm -vf "$id"
docker rmi rootfsimage

# Remove the archive direcgtory
rm -rf $PLUGIN_BASE_DIR

