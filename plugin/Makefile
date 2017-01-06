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
