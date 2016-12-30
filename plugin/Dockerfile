FROM centos:centos7.2.1511

RUN mkdir -p /run/docker/plugins /etc/docker/plugins /lcfs

WORKDIR /
COPY lcfs_plugin /

ENTRYPOINT ["/lcfs_plugin"]
