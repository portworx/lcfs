# Makefile for px-graph/plugin
# Maintainer Michael Vilain <michael@portworx.com> [201701.13]

BASE_DIR = $(shell git rev-parse --show-toplevel)
export GOPATH = $(shell echo ${BASE_DIR}/plugin/vendor | /bin/sed 's@\(.*\)/src/github.com.*@\1@g')
export GO15VENDOREXPERIMENT=1
ifndef PKGS
	PKGS := $(shell go list ./... 2>&1 | /bin/grep -v 'github.com/portworx/px-graph/vendor')
endif

TARGETS := lcfs_plugin

all: $(TARGETS)

lcfs_plugin: lcfs_plugin.go
	@go build -v -o lcfs_plugin

vendor-update:
	GO15VENDOREXPERIMENT=0 GOOS=linux GOARCH=amd64 go get -d -v -t -u -f $(shell go list ./... 2>&1 | grep -v 'github.com/portworx/px-graph/vendor')

vendor-without-update:
	go get -v github.com/kardianos/govendor
	rm -rf vendor
	govendor init
	GOOS=linux GOARCH=amd64 govendor add +external
	GOOS=linux GOARCH=amd64 govendor update +vendor
	GOOS=linux GOARCH=amd64 govendor add +external
	GOOS=linux GOARCH=amd64 govendor update +vendor

vendor: vendor-update vendor-without-update

vendor-install:
	go install -v $(PKGS)

clean:
	@rm -f lcfs_plugin


PLUGIN_BASE_DIR=artifacts
DOCKER_HUB_PXGRAPH_PLUGIN=px-graph
DOCKER_HUB_PXGRAPH_TAG=latest

# assume top-level Makefile built lcfs_plugin.bin
px-graph:
	if [ ! -e $(PLUGIN_BASE_DIR) ]; then mkdir -p $(PLUGIN_BASE_DIR) $(PLUGIN_BASE_DIR)/rootfs; fi
	cp config.json $(PLUGIN_BASE_DIR)/
	cp ../lcfs_plugin.bin lcfs_plugin
	docker build -t rootfsimage .
	docker create --name rootfsimage rootfsimage
	docker export rootfsimage | tar -x -C $(PLUGIN_BASE_DIR)/rootfs
ifeq ($(origin DOCKER_HUB_REPO), undefined)
	@echo "====================> creating plugin $(DOCKER_HUB_PXGRAPH_PLUGIN)"
	docker plugin create $(DOCKER_HUB_PXGRAPH_PLUGIN) $(PLUGIN_BASE_DIR)
else
    @echo "====================> creating plugin $(DOCKER_HUB_REPO)/$(DOCKER_HUB_PXGRAPH_PLUGIN):$(DOCKER_HUB_PXGRAPH_TAG)"
    docker plugin create $(DOCKER_HUB_REPO)/$(DOCKER_HUB_PXGRAPH_PLUGIN):$(DOCKER_HUB_PXGRAPH_TAG) $(PLUGIN_BASE_DIR)
endif
	docker plugin create $(DOCKER_HUB_REPO)/$(DOCKER_HUB_PXGRAPH_PLUGIN):$(DOCKER_HUB_PXGRAPH_TAG) $(PLUGIN_BASE_DIR)
	docker rm -vf rootfsimage
	docker rmi rootfsimage
	rm -rf $(PLUGIN_BASE_DIR)


push_plugin:
ifeq ($(origin DOCKER_HUB_REPO), undefined)
	@echo "please use 'make -e DOCKER_HUB_REPO=xxx -e DOCKER_HUB_PXGRAPH_PLUGIN=xxx -e DOCKER_HUB_PXGRAPH_TAG=xxx px-graph'"
else
    @echo "====================> Deploying container: docker push $(DOCKER_HUB_REPO)/$(DOCKER_HUB_PXGRAPH_PLUGIN):$(DOCKER_HUB_PXGRAPH_TAG)"
    #@docker login --username=$(DOCKER_HUB_USER) --password=$(DOCKER_HUB_PASSWD)
    docker plugin push $(DOCKER_HUB_REPO)/$(DOCKER_HUB_PXGRAPH_PLUGIN):$(DOCKER_HUB_PXGRAPH_TAG)
endif
