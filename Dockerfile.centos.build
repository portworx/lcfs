# vim:set ft=dockerfile ts=4:
# lcfs build Docker file [201701.06MeV]
# NOTE: this dockerfile does NOT create a runnable container
# it builds the lcfs and docker plugin binaries which will be copied out
# and run in it's own container with a private version of docker

FROM centos:centos7.2.1511
MAINTAINER Jose Rivera <jrivera@portworx.com>
ARG VERSION
ENV VERSION ${VERSION}

ARG REVISION
ENV REVISION ${REVISION}

ARG BUILD_FLAGS
ENV BUILD_FLAGS ${BUILD_FLAGS}

# tools to build 
RUN yum install -y make git rpm-build gcc gcc-c++ autoconf automake screen wget zlib-devel graphviz-devel 

ADD . /go/src/github.com/portworx/lcfs

WORKDIR /go/src/github.com/portworx/lcfs

RUN wget http://www.lttng.org/files/urcu/userspace-rcu-0.7.16.tar.bz2 && tar -xjf userspace-rcu-0.7.16.tar.bz2
WORKDIR userspace-rcu-0.7.16
RUN ./configure && make install

WORKDIR /go/src/github.com/portworx/lcfs
RUN wget -q https://github.com/libfuse/libfuse/releases/download/fuse-3.0.2/fuse-3.0.2.tar.gz && tar -xzf fuse-3.0.2.tar.gz
WORKDIR fuse-3.0.2
RUN cp ../fuse/fusermount.c util && ./configure --bindir=/opt/lcfs/bin && make -j8 && make install 

WORKDIR /go/src/github.com/portworx/lcfs
RUN wget -q http://yum.portworx.com/repo/rpms/lcfs/c7rpms.tgz && tar -xzf c7rpms.tgz
RUN rpm -Uvh rpmbuild/RPMS/noarch/pprof-2.4-8.el7.centos.noarch.rpm rpmbuild/RPMS/x86_64/gperftools-2.4-8.el7.centos.x86_64.rpm rpmbuild/RPMS/x86_64/gperftools-devel-2.4-8.el7.centos.x86_64.rpm rpmbuild/RPMS/x86_64/gperftools-libs-2.4-8.el7.centos.x86_64.rpm  rpmbuild/RPMS/x86_64/libunwind-1.1-5.el7.centos.2.x86_64.rpm rpmbuild/RPMS/x86_64/libunwind-devel-1.1-5.el7.centos.2.x86_64.rpm

WORKDIR /go/src/github.com/portworx/lcfs/lcfs

RUN make STATIC=y BUILD_FLAGS="${BUILD_FLAGS}" VERSION="${VERSION}" REVISION="${REVISION}" clean all

WORKDIR /go/src/github.com/portworx/lcfs
RUN mkdir -p /opt/lcfs/services && \
    \cp lcfs-setup.sh /opt/lcfs/bin && \
    \cp lcfs/lcfs /opt/lcfs/bin && \
    \cp lcfs.system* /opt/lcfs/services && \
    tar -C / -czvf /lcfs-$(lcfs/version_gen.sh -p)-centos.binaries.tgz opt

