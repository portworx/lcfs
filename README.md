# LCFS Storage Driver for Docker
Layer Cloning FileSystem (LCFS) is a new filesystem purpose-built to be a Docker [storage driver](https://docs.docker.com/engine/userguide/storagedriver/selectadriver/). All Docker images are constructed of “layers” using storage drivers (fma graph drivers) like AUFS, OverlayFS, and Device Mapper. As a design principle, LCFS focuses on “layers” as the first class citizen. By designing the LCFS filesystem for containers, the aim is to improve the container CI/CD and production experience. 

The future direction is to provide cluster-level operations, enhance container statistics, and pave the way towards content integrity of container images.

* *cluster operations*: where image pulls can be cooperatively satisfied by images across a group of servers instead of being isolated to a single server, as it is today. 
* *statistics*: answering what are the most popular layers, etc.
* *content-integrity*: ensuring container content has not been altered. [see OCI scope](https://www.opencontainers.org/about/oci-scope-table)

This LCFS driver is an open source project, and we welcome feedback and collaboration. At this point in time, the driver is at the experimental phase. 

# Design Principles 
Today, running containers on the same server is often limited by side-effects that come from mapping container behavior over generic filesystems. The approach impacts the entire lifecycle: building, launching, reading data, and exiting containers. 

Historically, filesystems have been built expecting that content is read/writeable. However, Docker images are constructed using many read-only layers and a single read-write layer. As more containers are launched of the same image (like apache/fedora), reading a file within a container requires traversing (up to) all the other containers running that image. 

The design principles are:
* *layers are first class*: inherently understand layers, the different state layers can take, and be able to directly track/manage layers.
* *clone independence*: create and run container images as independent entities-- from an underlying filesystem perspective. Each new instantiation of the same Docker image is an independent clone (at the read-only layer). 
* *containers in clusters*: optimize for clustered operations (cooperative 'pull'), optimize for common data patterns (writes to coalesce, data that is ephemeral), and avoid inheriting behavior that overlaps (when to use the graph-database).

# Efficiency Goals 
An internal (filesystem level) measure of success is making the create and management-- independent of the size of the image and the number of layers. An external measure of success is to make launching and terminating one hundred containers a constant time operation. 

Additional design goals are efficiency with:
* *page cache*: non-filesystem storage drivers will create multiple copies of the same image layers in memory. This leaves less host memory for containers. 
* *inodes*: some union filesystems will create multiple inodes to per file, leading to inode exhaustion in build scenarios and at scale.
* *space management*: just generally improve garbage collection and space management, automatically removing orphaned layers. 

# Deep Dive and Metrics 
// eric add

# Licensing
The Layer Cloning Filesystem (LCFS) is licensed under the Apache License, Verison 2.0. See [LICENSE](https://github.com/portworx/px-graph/blob/master/LICENSE) for the full license text.

# Contributing
Want to collaborate and add? Here are instructions to [get started contributing code](https://github.com/portworx/px-graph/blob/master/contributing.md). 

# Installing LCFS
The LCFS storage driver is available as a Docker v2 plugin and requires Docker version 1.13 or higher. It is available on the public Docker Hub and is installed using `docker plugin install --grant-all-permissions portworx/lcfs`.

### Building the LCFS plugin from source
You can run the [setup script](https://github.com/portworx/px-graph/tree/master/setup.sh) available in this directory to install the LCFS plugin.

## Building the LCFS plugin from source manually
Follow [these instructions](https://github.com/portworx/px-graph/tree/master/INSTALL.md) to install and configure lcfs manually.

