# LCFS Storage Driver for Docker
**_tl;dr:_** containers can be made faster to build, spin-up, spin-down, and to not bloat the host-- by innovating at the layer that boots-up containers. By designing the LCFS filesystem for Docker containers, the aim is to improve the speed of use, remove the manual maintenence and [workarounds](https://github.com/AkihiroSuda/issues-docker), and be available everywhere as a user-mode alternative. 

# Overview
Layer Cloning FileSystem (LCFS) is a new filesystem purpose-built to be a Docker [storage driver](https://docs.docker.com/engine/userguide/storagedriver/selectadriver/). All Docker images are constructed of layers using storage drivers (graph drivers) like AUFS, OverlayFS, and Device Mapper. As a design principle, LCFS focuses on layers as the first-class citizen. The LCFS filesystem operates directly on top of block devices, as opposed to over two filesystems that are then merged. Thereby, LCFS aims to directly manage at the container image’s layer level, eliminate the overhead of having a second filesystem that then is merged, and to optimize for density.

The future direction is to enhance LCFS with cluster-level operations, richer container statistics, and pave the way towards content integrity of container images.

* **cluster operations**: where image pulls can be cooperatively satisfied by images across a group of servers instead of being isolated to a single server, as it is today. 
* **statistics**: answering what are the most popular layers, and so on. 
* **content-integrity**: ensuring container content has not been altered. [See OCI scope.](https://www.opencontainers.org/about/oci-scope-table)

LCFS filesystem is an open source project, and we welcome feedback and collaboration. Currently, the driver is at the experimental phase. 

# Design Principles 
Today, running containers on the same server is often limited by side effects that come from mapping container behavior over general filesystems. The approach impacts the entire lifecycle: building, launching, reading data, and exiting containers. 

Historically, filesystems were built with the expectation that content is read/writeable. However, Docker images are constructed using many read-only layers and a single read-write layer. As more containers are launched of the same image (like Apache/Fedora), reading a file within a container requires traversing (up to) all the other containers running that image. 

The design principles are:
* **layers are managed directly**: inherently understand layers, their different states, and be able to directly track and manage layers.
* **clone independence**: create and run container images as independent entities, from an underlying filesystem perspective. Each new instantiation of the same Docker image is an independent clone, at the read-only layer. 
* **containers in clusters**: optimize for clustered operations (cooperative 'pull'), optimize for common data patterns (coalesce writes, data that is ephemeral), and avoid inheriting behavior that overlaps (when to use the graph-database).

# Performance Goals and Architecture 
An internal filesystem level measure of success is to make the creation and management independent of the size of the image and the number of layers. An external measure of success is to make the launch and termination of one hundred containers a constant time operation. 

Additional performance considerations:
* **page cache**: non-filesystem storage drivers create multiple copies of the same image layers in memory. This leaves less host memory for containers. We should not.
* **inodes**: some union filesystems create multiple inodes per file, leading to inode exhaustion in build scenarios and at scale. We should not. 
* **space management**: a lot can be done to improve garbage collection and space management, automatically removing orphaned layers. We should do this. 

## Measured Performance
The current experimental release of LCFS is shown  against several of the top storage drivers. The tests used to generate these results are here. 

**Create / Destroy**: The diagram below depicts the time to [create](https://docs.docker.com/engine/reference/run/) and [destroy](https://docs.docker.com/engine/reference/commandline/rm/) 100 Apache/Fedora containers. The cumulative time measured: LCFS at 44 seconds, Overlay at 237 sec, Overlay2 at 245 sec, and Device Mapper at 556 sec. 
![alt text](http://i.imgur.com/JSUeqLc.png "create and destroy times")


**Build**: The diagram below depicts the time to build containers using various storage drivers. The individual times measured: Device Mapper at 1511 sec, Overlay at 913 seconds, Overlayv2 at 567 seconds, and LCFS at 819 seconds. Future work for LCFS is improving the differencing mechanism to improve this scenario. 
![alt text](http://i.imgur.com/QAUsMI4.jpg "build times")

## Architecture 
The LCFS filesystem is user-level, written in C, POSIX-compliant, and integrated into Linux and macOS via Fuse. It does not require any kernel modifications, enabling it to be portable filesystem. 

To start to explain the layers-first design, let us compare launching three containers that use Fedora. In the following diagram, the left side shows a snapshot based storage driver (like Device Mapper or Btrfs). Each container boots from its own read-write (rw) layer and read-only (init) layers. Good. 

However, OS filesystems have been historically built to expect content to be read-writeable, often using [snapshots](https://github.com/portworx/lcfs/blob/master/docs/layers_overview.md#snapshots-in-other-drivers-and-clones-in-lcfs) of the first container’s init layer to create the second and third. One side effect becomes that almost all operations from the third container then must traverse the lower init layers of the first two containers’ layers. This leads to a slow down for nearly all file operations as more containers are launched, including reading from a file. 

![alt text] (http://i.imgur.com/vxv3FUW.png "LCFS vs Snapshot driver diagram")

In the above  diagram, the right side shows that LCFS also presents a unified view (mount) for three containers running the Fedora image. The design goal is to unchain how containers access their own content. First, launching the second container results in a new init [clone (not a snapshot)](https://github.com/portworx/lcfs/blob/master/docs/layers_overview.md#snapshots-in-other-drivers-and-clones-in-lcfs). Internally, the access of the second container’s (init) filesystem does not require tracking backwards to an original (snapshot’s) parent. The net effect is that read and modify operations from successive containers do not depend on prior containers. 

Separately, LCFS itself is implemented as a single filesystem. It takes in (hardware) devices and puts one filesystem over those drives. For more on the LCFS architecture and future TODOs, please see: 

* [*caching*](https://github.com/portworx/lcfs/blob/master/docs/caching_overview.md): how LCFS caches and accesses metadata (inodes, directories, etc) and future work. 
* [*crash consistency*](https://github.com/portworx/lcfs/blob/master/docs/crashconsistency_overview.md): work in progress. 
* [*file operations*](https://github.com/portworx/lcfs/blob/master/docs/file_operations.md): how LCFS supports file operations and file locking.
* [*layers*](https://github.com/portworx/lcfs/blob/master/docs/layers_overview.md): how layers are created, cloning versus snapshotting, differenced, and locked. 
* [*layout*](https://github.com/portworx/lcfs/blob/master/docs/layout_overview.md): how LCFS formasts devices, handles inodes, and handles internal data structures. 
* [*space management*](https://github.com/portworx/lcfs/blob/master/docs/spacemanagement_overview.md): how LCFS handles allocation, tracking, placement, and I/O coalescing. 
* [*stats*](https://github.com/portworx/lcfs/blob/master/docs/stats_overview.md): how to access stats and future work. 


# Installing LCFS
First, install the LCFS filesystem and then the LCFS v2 graph driver plugin, as described in [these instructions](INSTALL.md).

# Licensing
The Layer Cloning Filesystem (LCFS) is licensed under the Apache License, Version 2.0. See [LICENSE](https://github.com/portworx/px-graph/blob/master/LICENSE) for the full license text.

# Contributing
Want to collaborate and add? Here are instructions to [get started contributing code](https://github.com/portworx/px-graph/blob/master/contributing.md). 
