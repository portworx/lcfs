# vim:set ft=dockerfile ts=4:
# lcfs build Docker file [201701.06MeV]
# NOTE: this dockerfile does NOT create a runnable container
# it builds the lcfs and docker plugin binaries which will be copied out
# and run in it's own container with a private version of docker

FROM golang
MAINTAINER Jose Rivera <jrivera@portworx.com>
ARG VERSION
ENV VERSION ${VERSION}

ARG REVISION
ENV REVISION ${REVISION}

ARG BUILD_FLAGS
ENV BUILD_FLAGS ${BUILD_FLAGS}

# tools to build libfuse for lcfs
RUN apt-get update && \
    apt-get install -y build-essential util-linux libcurl4-openssl-dev \
                       libxml2-dev mime-support libgoogle-perftools-dev liblzma-dev rpm file alien sudo libz-dev liburcu-dev
ADD . /go/src/github.com/portworx/lcfs

WORKDIR /go/src/github.com/portworx/lcfs
RUN wget -q https://github.com/libfuse/libfuse/releases/download/fuse-3.0.2/fuse-3.0.2.tar.gz && \
    tar -xzf fuse-3.0.2.tar.gz
WORKDIR fuse-3.0.2
RUN cp ../fuse/fusermount.c util && ./configure --bindir=/opt/lcfs/bin && make -j8 && make install && cp -a /opt/lcfs/bin/fusermount3 /go/src/github.com/portworx/lcfs/lcfs

WORKDIR /go/src/github.com/portworx/lcfs/lcfs
RUN make clean && \
    make STATIC=y BUILD_FLAGS="${BUILD_FLAGS}" VERSION="${VERSION}" REVISION="${REVISION}" rpm
RUN mkdir /tmp/pkgs && \
    \cp -a rpm/lcfs/RPMS/x86_64/*.deb /tmp/pkgs && \
    \cp -a rpm/lcfs/RPMS/x86_64/*.rpm /tmp/pkgs && \
    \cp -a rpm/lcfs/RPMS/x86_64/*.tgz /tmp/pkgs && \
    \cp ../fuse-3.0.2/util/fusermount3 /tmp

WORKDIR /go/src/github.com/portworx/lcfs/plugin
RUN make clean && make && \
    mv lcfs_plugin /tmp/lcfs_plugin.bin
