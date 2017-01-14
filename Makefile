# Makefile for px-graph
# Maintainer Michael Vilain <michael@portworx.com> [201701.13]
# assumes make is run as root or account running it is part of Docker group

.PHONY : gr-build gr-clean

GR_CONTAINER:=px-graph

ifdef LCFS_RPM_VERSION
BUILD_ARGS=--build-arg LCFS_RPM_VER=$(LCFS_RPM_VERSION)
endif

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
	docker build -t $(GR_CONTAINER) $(BUILD_ARGS) -f Dockerfile.build .
	docker run --name $(GR_CONTAINER) $(GR_CONTAINER) ls -l /tmp
	docker cp $(GR_CONTAINER):/tmp/lcfs_plugin.bin .
	docker cp $(GR_CONTAINER):/tmp/lcfs.bin .
	docker cp $(GR_CONTAINER):/tmp/pkgs .
	docker rm $(GR_CONTAINER)

gr-clean:
	@echo "removing $(REPO)$(GR_CONTAINER)"
	-docker rm -vf $(REPO)$(GR_CONTAINER)
	-docker rmi $(GR_CONTAINER)

plugin:
	@cd plugin && make

px-graph:
	@echo "====================> building px-graph docker plugin..."
	cd plugin/ && make px-graph   #./build_plugin.sh

deploy:
	@echo "====================> pushing px-graph to dockerhub..."
	@cd plugin/ && make push_plugin  #./push_plugin.sh


plugin-clean:
	@cd plugin && make clean

vendor:
	@cd plugin && make vendor

vendor-install:
	@cd plugin && make vendor-install
