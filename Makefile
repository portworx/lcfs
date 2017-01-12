# Makefile for px-graph
# Maintainer Michael Vilain <michael@portworx.com> [201701.05]

# these can be overridden at the command line with -e DOCKER_HUB_USER=wilkins
ifeq ($(origin DOCKER_HUB_REPO), undefined)
	REPO=
else
	REPO:= $(DOCKER_HUB_REPO)/
endif

.PHONY : gr-build gr-clean

GR_CONTAINER:=px-graph

TARGETS := gr-docker plugin

all: $(TARGETS)

build: gr-plugin

clean: gr-clean plugin-clean

run : 

submodules:
	git submodule init
	git submodule update

# build px-graph plugin in a container (2017.05 only works with docker 1.13)
gr-plugin:
	@echo "====================> building px-graph build container $(GR_CONTAINER)"
	docker build -t $(GR_CONTAINER) -f Dockerfile.build .
	docker run --name $(GR_CONTAINER) $(GR_CONTAINER) ls -l /tmp
	docker cp $(GR_CONTAINER):/tmp/lcfs_plugin.bin .
	docker cp $(GR_CONTAINER):/tmp/lcfs.bin .
	docker rm $(GR_CONTAINER)

gr-clean:
	@echo "removing $(REPO)$(GR_CONTAINER)"
	-docker rm -vf $(REPO)$(GR_CONTAINER)
	-docker rmi $(GR_CONTAINER)

plugin:
	@cd plugin && make

plugin-clean:
	@cd plugin && make clean

vendor:
	@cd plugin && make vendor

vendor-install:
	@cd plugin && make vendor-install
