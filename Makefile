# Makefile for px-graph
# Maintainer Michael Vilain <michael@portworx.com> [201612.28]

# these can be overridden at the command line with -e DOCKER_HUB_USER=wilkins
ifeq ($(origin DOCKER_HUB_REPO), undefined)
	REPO=
else
	REPO:= $(DOCKER_HUB_REPO)/
endif

.PHONY : gr-build gr-clean

GR_CONTAINER:=px-graph

TARGETS := gr-docker

all: $(TARGETS)

build: docker

clean: gr-clean

deploy: 

run : 

submodules:
	git submodule init
	git submodule update

DOCKER_VERS=12
# pull docker and build it with px-graph components in it
docker:
	if [ -d docker$(DOCKER_VERS) ]; then rm -rf docker$(DOCKER_VERS); fi
	git clone -b 1.$(DOCKER_VERS).x git@github.com:docker/docker docker$(DOCKER_VERS)
	mkdir docker$(DOCKER_VERS)/daemon/graphdriver/lcfs
	cp -v docker.1.$(DOCKER_VERS)/daemon/graphdriver/lcfs/* docker$(DOCKER_VERS)/daemon/graphdriver/lcfs
	cp -v docker.1.$(DOCKER_VERS)/daemon/graphdriver/register/register_lcfs.go docker$(DOCKER_VERS)/daemon/graphdriver/register
	#git add --all
	# uses docker container to build docker with their toolset; won't run on MacOS
	cd docker$(DOCKER_VERS) && make build && make binary
	# sudo service docker stop
	# sudo cp bundles/latest/binary-client/docker /usr/bin
	# sudo cp bundles/latest/binary-daemon/dockerd /usr/bin/dockerd
	# sudo cp bundles/latest/binary-daemon/docker-runc /usr/bin
	# sudo cp bundles/latest/binary-daemon/docker-containerd /usr/bin
	# sudo cp bundles/latest/binary-daemon/docker-containerd-ctr /usr/bin
	# sudo cp bundles/latest/binary-daemon/docker-containerd-shim /usr/bin
	# add "-s lcfs" as option in /usr/lib/systemd/system/docker.service to dockerd line
	# sudo service docker start

# build px-graph components in a container (201612.28 not currently working)
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