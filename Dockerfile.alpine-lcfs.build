# vim:set ft=dockerfile ts=4:
# lcfs build Docker file [201701.06MeV]
# NOTE: this dockerfile does NOT create a runnable container
# it builds the lcfs and docker plugin binaries which will be copied out
# and run in it's own container with a private version of docker

FROM alpine
MAINTAINER Jose Rivera <jrivera@portworx.com>
ARG VERSION
ENV VERSION ${VERSION}

ARG REVISION
ENV REVISION ${REVISION}

ARG BUILD_FLAGS
ENV BUILD_FLAGS ${BUILD_FLAGS}

# tools to build libfuse for lcfs
RUN apk update && \
    apk add build-base gcc abuild binutils binutils-doc gcc-doc util-linux pciutils usbutils coreutils binutils findutils grep alpine-sdk automake  m4 autoconf libtool linux-headers zlib-dev userspace-rcu-dev libunwind-dev gdb

ADD . /go/src/github.com/portworx/lcfs

WORKDIR /go/src/github.com/portworx/lcfs
RUN wget -q https://github.com/libfuse/libfuse/releases/download/fuse-3.0.2/fuse-3.0.2.tar.gz && \
    tar -xzf fuse-3.0.2.tar.gz

WORKDIR fuse-3.0.2
RUN cp -a ../fuse/fusermount.c util && cp -a ../fuse/lib/* lib && ./configure CFLAGS=-D__MUSL__ --bindir=/opt/lcfs/bin && make -j8 && make install && cp -a /opt/lcfs/bin/fusermount3 /go/src/github.com/portworx/lcfs/lcfs

WORKDIR /go/src/github.com/portworx/lcfs/gperftools
RUN ./autogen.sh  && ./configure --enable-minimal && make install 

WORKDIR /go/src/github.com/portworx/lcfs/lcfs
RUN make STATIC=y BUILD_FLAGS="${BUILD_FLAGS}" VERSION="${VERSION}" REVISION="${REVISION}" clean all

WORKDIR /go/src/github.com/portworx/lcfs
RUN mkdir -p /opt/lcfs/services && \
    \cp lcfs-setup.sh /opt/lcfs/bin && \
    \cp lcfs/lcfs /opt/lcfs/bin && \
    \cp lcfs.system* /opt/lcfs/services && \
    tar -C / -czvf /lcfs-$(lcfs/version_gen.sh -p)-alpine.binaries.tgz opt
