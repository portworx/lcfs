# LCFS Storage Driver for Docker
**_tl;dr:_** containers can be made faster to build, spin-up, spin-down, and to not bloat the host-- by innovating at the layer that boots-up containers. By designing the LCFS filesystem for Docker containers, the aim is to improve the speed of use, remove the manual maintenence and [workarounds](https://github.com/AkihiroSuda/issues-docker), and be available everywhere as a user-mode alternative. 

# Overview
Layer Cloning FileSystem (LCFS) is a new filesystem purpose-built to be a Docker [storage driver](https://docs.docker.com/engine/userguide/storagedriver/selectadriver/). All Docker images are constructed of “layers” using storage drivers (fma graph drivers) like AUFS, OverlayFS, and Device Mapper. As a design principle, LCFS focuses on “layers” as the first class citizen. 

The future direction is to provide cluster-level operations, enhance container statistics, and pave the way towards content integrity of container images.

* **cluster operations**: where image pulls can be cooperatively satisfied by images across a group of servers instead of being isolated to a single server, as it is today. 
* **statistics**: answering what are the most popular layers, etc.
* **content-integrity**: ensuring container content has not been altered. [see OCI scope](https://www.opencontainers.org/about/oci-scope-table)

This LCFS driver is an open source project, and we welcome feedback and collaboration. At this point in time, the driver is at the experimental phase. 

# Design Principles 
Today, running containers on the same server is often limited by side-effects that come from mapping container behavior over generic filesystems. The approach impacts the entire lifecycle: building, launching, reading data, and exiting containers. 

Historically, filesystems have been built expecting that content is read/writeable. However, Docker images are constructed using many read-only layers and a single read-write layer. As more containers are launched of the same image (like apache/fedora), reading a file within a container requires traversing (up to) all the other containers running that image. 

The design principles are:
* **layers are first class**: inherently understand layers, the different state layers can take, and be able to directly track/manage layers.
* **clone independence**: create and run container images as independent entities-- from an underlying filesystem perspective. Each new instantiation of the same Docker image is an independent clone (at the read-only layer). 
* **containers in clusters**: optimize for clustered operations (cooperative 'pull'), optimize for common data patterns (writes to coalesce, data that is ephemeral), and avoid inheriting behavior that overlaps (when to use the graph-database).

# Performance Goals and Architecture 
An internal (filesystem level) measure of success is making the create and management-- independent of the size of the image and the number of layers. An external measure of success is to make launching and terminating one hundred containers a constant time operation. 

Additional performance considerations:
* **page cache**: non-filesystem storage drivers will create multiple copies of the same image layers in memory. This leaves less host memory for containers. We should not.
* **inodes**: some union filesystems will create multiple inodes to per file, leading to inode exhaustion in build scenarios and at scale. We should not. 
* **space management**: a lot can be done to improve garbage collection and space management, automatically removing orphaned layers. We should do this. 

## Measured Performance
The current experimental release of LCFS is shown  against several of the top storage drivers. The tests used to generate these results are here. 

**Create / Destroy**: The below depicts the time to [create](https://docs.docker.com/engine/reference/run/) and [destroy](https://docs.docker.com/engine/reference/commandline/rm/) 100 containers. The cumlative time measured: LCFS at 44 seconds, Overlay at 237 sec, Overlay2 at 245 sec, and Device Mapper at 556 sec (not plotted). 
![alt text](http://i.imgur.com/H3Eppc4.png "create and destroy times")


**Build**: The below depicts the time to build containers using various storage drivers. The individual times measured: Device Mapper 1511 sec, Overlay 913 seconds, Overlayv2 567 seconds, and LCFS 819 seconds. Future work for LCFS is improving the differencing mechanism to improve this scenario. 
![alt text](http://i.imgur.com/QAUsMI4.jpg "build times")

## Architecture 
The LCFS filesystem is user-level, written in C, POSIX-compliant, and integrated into Linux and MacOS via Fuse. It does not require any kernel modifications, enabling it to be portable filesystem. 

To start to explain the layers-first design, let us compare launching three containers that use Fedora. In the lower left, a general diagram of a filesystem storage driver (like AUFS or OverlayFS) can be depicted as two file systems (shown) that get merged. Each container boots frow its own read-write (rw) layer and read-only (init) layers. Good. 

However, OS filesystems have been historically built to expect content to be read-writeable, often using [snapshots](https://github.com/portworx/lcfs/blob/master/docs/layers_overview.md#snapshots-in-other-drivers-and-clones-in-lcfs) of the first container’s init layer to create the second and third. One side effect becomes that almost all operations from the third container now have to traverse the lower init layers of the first two containers’. This leads to slow down for all file operations as more containers are launched, slow down applies to read and not just modifying content. 

![alt text] (http://i.imgur.com/H0VpEkK.png "architecture diagram")

In the above right, LCFS must also present a unified view (mount) for three containers running the Fedora image. The design goal is to unchain how containers access their own content. First, launching the second container results in a new init [clone (not a snapshot)](https://github.com/portworx/lcfs/blob/master/docs/layers_overview.md#snapshots-in-other-drivers-and-clones-in-lcfs). Internally, the access of the second container’s (init) filesystem does not require tracking backwards to an original (snapshot’s) parent. The net effect is that reads and modify from successive containers do not depend on prior containers. 

Separately, LCFS itself is implemented as a single filesystem. It takes in (hardware) devices and puts one filesystem over those drives. For more on the LCFS architecture and future TODOs, please see: 

* [*caching*](https://github.com/portworx/lcfs/blob/master/docs/caching_overview.md): how metadata (inodes, directories, etc) are cached, accessed, and future work. 
* [*crash consistency*](https://github.com/portworx/lcfs/blob/master/docs/crashconsistency_overview.md): work in progress. 
* [*file operations*](https://github.com/portworx/lcfs/blob/master/docs/file_operations.md): how file operations are supported and how files are locked. 
* [*layers*](https://github.com/portworx/lcfs/blob/master/docs/layers_overview.md): how layers are created, cloning versus snapshotting, differenced, and locked. 
* [*layout*](https://github.com/portworx/lcfs/blob/master/docs/layout_overview.md): how devices are formatted, inodes handled, and internal data structures. 
* [*space management*](https://github.com/portworx/lcfs/blob/master/docs/spacemanagement_overview.md): how allocation, tracking, placement, and I/O coalescing are handled. 
* [*stats*](https://github.com/portworx/lcfs/blob/master/docs/stats_overview.md): how to access stats and future work. 


# Installing LCFS
Installing LCFS involves installing the LCFS file system and then the LCFS v2 graph driver plugin.
Follow [these instructions](INSTALL.md) to install the LCFS plugin.

# Licensing
The Layer Cloning Filesystem (LCFS) is licensed under the Apache License, Verison 2.0. See [LICENSE](https://github.com/portworx/px-graph/blob/master/LICENSE) for the full license text.

# Contributing
Want to collaborate and add? Here are instructions to [get started contributing code](https://github.com/portworx/px-graph/blob/master/contributing.md). 
