#!/bin/bash

SUDO=sudo

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


# Push the plugin to docker hub
$SUDO docker plugin push $DOCKER_HUB_REPO/$DOCKER_HUB_PXGRAPH_PLUGIN:$DOCKER_HUB_PXGRAPH_TAG
