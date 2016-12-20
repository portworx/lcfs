# Makefile for px-graph -- a containerized build for px-graph driver
# Maintainer Michael Vilain <michael@portworx.com> [201612.20]

# these can be overridden at the command line with -e DOCKER_HUB_USER=wilkins
ifeq ($(origin DOCKER_HUB_REPO), undefined)
	REPO=
else
	REPO:= $(DOCKER_HUB_REPO)/
endif

.PHONY : gr-build gr-clean

GR_CONTAINER:=px-graph

TARGETS := gr-build

all: $(TARGETS)

build: gr-build

clean: gr-clean

deploy: 

run : 

submodules:
	git submodule init
	git submodule update

# build px-graph components
gr-build:
	@echo "====================> building px-graph build container $(GR_CONTAINER)_tmp"
	docker build -t $(GR_CONTAINER) -f Dockerfile.build .
	docker run --name $(GR_CONTAINER) $(GR_CONTAINER) ls -l /tmp
	docker cp $(GR_CONTAINER):/tmp/lcfs2.bin .
	docker cp $(GR_CONTAINER):/tmp/lcfs_plugin2.bin .
	docker cp $(GR_CONTAINER):/tmp/lcfs3.bin .
	docker cp $(GR_CONTAINER):/tmp/lcfs_plugin3.bin .
	docker rm $(GR_CONTAINER)

gr-clean:
	@echo "removing $(REPO)$(GR_CONTAINER)"
	-docker rm -vf $(REPO)$(GR_CONTAINER)
	-docker rmi $(GR_CONTAINER)_tmp