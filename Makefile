# Makefile for lcfs
# Maintainer Jose Rivera <jrivera@portworx.com> [201701.16]
# assumes make is run as root or account running it is part of Docker group

.PHONY : gr-build gr-clean

GR_CONTAINER:=lcfs

ifdef VERSION
override BUILD_ARGS := $(BUILD_ARGS) --build-arg VERSION="$(VERSION)"
endif

ifdef REVISION
override BUILD_ARGS := $(BUILD_ARGS) --build-arg REVISION="$(REVISION)"
endif

ifdef BUILD_FLAGS
override BUILD_ARGS := $(BUILD_ARGS) --build-arg BUILD_FLAGS="$(BUILD_FLAGS)"
endif

TARGETS := gr-plugin

all: $(TARGETS)

build: gr-plugin

clean: gr-clean plugin-clean
	rm -rf lcfs.bin lcfs_plugin.bin
	-\rm -rf pkgs
	-\rm -rf lcfs/version/* lcfs/rpm/lcfs lcfs/rpm/rpmtmp lcfs/rpm/tmp     # Unable to \rm -rf. Docker bug so remove directories manually

run :

submodules:
	git submodule init
	git submodule update

# build lcfs plugin in a container (2017.05 only works with docker 1.13)
gr-plugin:
	@echo "====================> building lcfs build container $(GR_CONTAINER)"
	 docker build -t $(GR_CONTAINER) $(BUILD_ARGS) -f Dockerfile.build .
	 docker run --name $(GR_CONTAINER) $(GR_CONTAINER) ls -l /tmp
	 docker cp $(GR_CONTAINER):/tmp/lcfs_plugin.bin .
	 docker cp $(GR_CONTAINER):/tmp/pkgs .
	 docker cp $(GR_CONTAINER):/tmp/fusermount .
	 docker rm $(GR_CONTAINER)

gr-clean:
	@echo "removing $(REPO)$(GR_CONTAINER)"
	-docker rm -vf $(REPO)$(GR_CONTAINER)
	-docker rmi $(GR_CONTAINER)

plugin:
	@cd plugin && make

lcfs:
	@echo "====================> building lcfs docker plugin..."
	cd plugin/ && make lcfs #./build_plugin.sh

deploy:
	@echo "====================> pushing lcfs to dockerhub..."
	@cd plugin/ && make push_plugin  #./push_plugin.sh


plugin-clean:
	@cd plugin && make clean

vendor:
	@cd plugin && make vendor

vendor-install:
	@cd plugin && make vendor-install
