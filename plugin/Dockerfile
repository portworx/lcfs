FROM centos:centos7.2.1511

RUN mkdir -p /run/docker/plugins
RUN mkdir -p /etc/docker/plugins

WORKDIR /
COPY lcfs_plugin /

ENTRYPOINT ["/lcfs_plugin"]
