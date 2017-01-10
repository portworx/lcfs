# About the Portworx Graph Driver for Docker
PX-Graph is a graph driver for Docker and designed to provide a purpose-built container image management file system. It is specifically designed to address the following issues:

1. Efficient page cache usage: Current graph drivers that depend on device mapper abuse the page cache by loading multiple copies of the same image layers in memory. This takes away host memory from running applications.
2. Efficient i-node usage: Current graph drivers exhaust the number of inodes available, thereby causing the underlying filesystems to run out of space.
3. Efficient cloning: Current graph drivers such as overlay implement a copy-on-write approach, which consumes CPU and takes time during container image management operations
4. Efficient and correct garbage collection and space management: Current graph drivers routinely end up with orphaned layers and cause the operator to resort to resetting Docker (usually by deleting `/var/lib/docker`).

PX-Graph is built on a new filesystem designed by Portworx, specifically for managing Linux container images. This filesystem is called LCFS, which stands for `Layer Cloning File System`.

This file system is provided to Docker by way of the FUSE low level API. 

# Installing the Portworx Graph driver for Docker
PX-Graph is available as a v2 plugin and requires Docker version 1.13 or higher. It is available on the public Docker hub and is installed using `docker plugin install portworx/px-graph`.

> Currently the v2 interface is not generally available. There is an [oustanding issue with the v2 interface](https://github.com/docker/docker/issues/28948). Therefore, Portworx provides an alternate way of installing the PX-Graph plugin.  
> Follow [these instructions](https://github.com/portworx/px-graph/tree/master/INSTALL.md) to install Px-Graph


# Overview

Portworx Graph Driver is a custom-built file system to meet the needs of a saving, starting and managing Linux container images.  In the context of Docker, it is similar to AUFS and Overlay graph drivers in terms of it's use cases.  Unlike AUFS and Overlay, this new graph driver is a native file system, which means it does not operate on top of another file system, but operates directly on top of block devices. Therefore, this file system does not have the inefficiencies of merged file systems or device-mapper based systems.

This new file system is a user-level file system written in C and integrated into Linux and MacOS via Fuse.  Therefore it does not require any kernel modifications, making it a portable file system. It is a POSIX-compliant file system. Given the ephemeral (temporary) nature of data stored in a graph driver, it is implemented without having some of the complexities of a general-purpose file system (for example, journaling). Most file systems in use today are optimized towards persistent data, provide ACID properties for system calls and attempts to work well with random read-write workloads.  This file system is written with container image handling as a specific workload.  These operations involve:

1. Container image creation
2. Container image cloning and launcing of instances
3. Container image memory consumption
4. Number of inodes reported to the kernel by way of multiple copies of the same image (or layers) running
5. Container image data management - Actions like deletion, forced image removal nd local system resource usage based on multiple container images being present

A lot of these techniques are implemented using image specific snapshotting techniques that are optimized for page cache consumption, image snap creation time and inode count.  Similar to other graph drivers, this graph driver also creates layers for images and read-write layers on top of those for containers.  Each image will have a base layer, in which files of the image are populated initially. Additional layers are created on top of the base layer and for each additional layer in the image being extracted.  Each layer shares data from the previous layer.  If a layer modifies any data, that data is visible from the layers on top of that layer, but not from the layers below that layer. Also if a container modifies data in an image it is loaded from, that modified data is not visible from any other derived layers on top of that layer.

A layer in an image is a read-only snapshot sitting on top of the previous layer in the image.  Therefore, these derived layers share common data between each other.  A layer is immutable after it's contents are completely populated.  When any existing data inherited from a previous layer is modified while populating data to a new layer, a branch-on-write (BOW) operation is performed in increments of 4KB blocks.  New data will be written to a newly allocated location on the back-end block storage device, and old data will no longer be accessible from the layer (or any other layer created on top of that subsequently) which modified the data.  Similarly, any files deleted from a layer are not visible from that layer or on any of the layers on top of that layer.

When a read-write layer is created while starting a new container (two such layers for every container), a read-write snapshot is created on top of the image layer and mounted from the container. The container can see all the data from the image layers below the read-write layer and can create new data or modify any existing data as needed. As it may be clear by now, when any existing data is modified, the data is not modified in the image layer... instead a private copy with new data is made available for the read-write layer.

Traditional file systems need to provide ACID properties for every system call, but for a graph driver, that is required only when a layer is created, deleted or persisted. This graph driver is hosting the Docker database with information about various images and containers.  It ensures that the database is consistent with the images and therefore the image data can be read correctly regardless of restarts or crashes.  This design inturn avoids you to have to externally monitor or garbage inspect `/var/lib/docker`.

Snapshots are implemented without using any reference counts and thus support unlimited number of layers. The time to create a snapshot is independent of the size of the file system (devices), size of the data set, or the number of layers present in the file system. Snapshots are deleted in the background and processing time depends on the amount of data actually created/modified in the snapshot. Thus creation and deletion of layers can be done instantaneously.

The layers on which new layers are created are read-only after they are populated, and a new layer will not conflict with any modification operations in progress on the parent layer. That is not the case with snapshots in traditional file systems. In a graph driver, creating a new layer or removing a layer does not have to stop any in-progress operations and thus creating/removing images and containers can proceed without any noticeable impact on any other running containers.

Operations within a layer is independent of the total number of layers present in the file system. Many snapshot implementations in other file systems may fail on this operation, since snapshots are point-in-time images and snapshots may form a chain. Therefore, operations on older snapshots may be negatively impacted.

Unlike a traditional file system, layers in a graph are deleted in the reverse order those are created. The newest layer is deleted first, and then the one created just before it. A layer in the middle of chain or the base layer cannot be deleted when there is a newer layer on top of that layer still around. This simplifies the overall snapshot design since deleting a snapshot in the middle/beginning of the chain is a lot more complex to get working. For example, each layer easily track space allocated for storing data created/modified by the layer and any such space can be freed without worrying about some other layer sharing any such data.

Also layers are not for rolling back, thus it does not have to incur some of the complexities of snapshots in a traditional file system. There is also no need to provide any block level differences between any two layers.

## Layout

When a new device is formatted as a new graph driver file system, a superblock is placed with some file system specific information at the beginning of the device. This information helps to recognize this device to have a valid file system on it anytime it is mounted again in the future. If a device with no valid superblock is mounted as this file system, it is formatted before mounting.

Similarly, each of the layers created in the file system also has a private superblock for locating data which belongs exclusively to that layer. Each layer in the file system has a unique index. This index stays same for the life time of the layer.

In addition to the layers created for storing images and containers, there is a global file system layer which keeps data not part of any layers. This layer always has the index of 0. This layer cannot be deleted.

Superblocks of layers taken on a top of a common layer are linked together. Superblocks of the common layer point to one of those top layer superblocks. Thus superblocks of all layers taken on top of a layer can be reached from the superblock of that common bottom layer.

Available space is tracked using a list of free extents. There will be a single such extent immediately after the file system is formatted. The superblock of layer 0 tracks the blocks where this list is stored. Similarly, all other layers keep track of extents allocated to those layers. Those blocks are also reachable from the superblock of those layers.

4KB is the smallest unit of space allocation or size of I/O to the device, called file system block size. For files larger than 4KB, multiple such blocks can be allocated in a single operation. Every layer shares the whole device, and space can be allocated for any layer from anywhere in the underlying device.

Each file created in any layer has an inode to track information specific to that file such as stat info, dirty data not flushed to disk, etc. Each inode has a unique identifier in the file system called “inode number.” Files deleted in a layer do not have to maintain any whiteouts as in some union file systems, as their references from the directories are removed in that layer. Inode numbers are not reused even after a file is deleted.

All UNIX file types are supported. For symbolic links, the target name is also stored in the same block where inode is written. For directories, separate blocks are allocated for storing directory entries and those blocks are linked together as chain and the chain is linked from the inode. For regular files, additional blocks are allocated for storing data and linked from the inode. When a file becomes fragmented, i.e., when an entire file cannot be stored contiguously on disk, then additional blocks are allocated to track file page offsets and corresponding disk locations where data is stored, in extent format. Such blocks, called “emap blocks,” are linked from the inode as well. If the file has extended attributes, those are stored in additional blocks and linked from the inode as well. Currently, directories, emap blocks, and extended attribute blocks keep entries as a sequential lists, and they should be switched to use better data structures like B-trees, etc., in future.

All inodes in a layer can be reached from the superblock of the layer. Every inode block is tracked in blocks linked from the superblock. Inodes are not stored in any particular order on disk ,and inodes have their number within the inode.

All metadata (superblocks, inodes, directories, emap, extended attributes, etc.) are always cached in memory (although this may change in the future). They are read from disk when file system is mounted and written out when file system is unmounted.

The root directory of the file system has inode number 2 and cannot be removed. Anything created under tmp directory in root directory is considered temporary.

### Layer root directory

There is another directory under which roots of all layers are placed and called “layer root directory.” This directory also cannot be removed once created. This directory is for internal use and creating files in this directory is not allowed.

### File handles

File handles are formed by combining the layer index and the inode number of the file. This is a 64-bit number and is returned to FUSE when files are opened / created. This file handle can be used to locate the same file in subsequent operations like read, readdir, write, truncate, flush, release, etc. The file handle for a shared file, when accessed from different layers, would be different as the layer index part of the file handle would be different. This may turn out to be problem when same file is read from different layers as multiple copies of data may end up in the kernel page cache – in order to alleviate that problem, pages of a shared file in kernel page cache are invalidated on last close of a shared file. Also FUSE is requested to invalidate pages of a shared file on every open (this should be done when a file is closed, but FUSE does not have any controls for doing this as of today). Also the direct-mount option should not be used since it would prevent mmap. Ideally, FUSE should provide an option to bypass pagecache for a file when the file is not mmapped.

### Locking

Each layer has a read-write lock, which is taken in shared mode while reading/writing to the layer (all file operations). This lock is taken in exclusive mode while unmounting the root layer or while deleting any other layer.

Each inode has a read-write lock. Operations which can be run in shared mode (read, readdir, getattr, etc.), take that lock in shared mode, while other operations which modify the inode hold that lock in exclusive mode. This lock is not taken once a layer is frozen (meaning, a new layer is created on top of that layer and no more changes are allowed in the layer).

### Layers

New layers are added after locking the parent layer in shared mode, if there is a parent layer. The newly created layer will be linked to the parent layer. All the layers taken on a parent layer are linked together as well.

A layer with no parent layer forms a base layer. Base layer for any layer can be reached by traversing the parent layers starting from that layer. All layers with same base layer form a “tree of layers”.

A layer is removed after locking that layer in exclusive mode. That makes sure all operations on that layer are drained. Also shared locks on the base layer is held during that operation.

Root layer is locked in shared mode while creating/deleting layers. Root layer is locked exclusive while unmounting the file system.

### Space management/reclamation

Each layer will allocate space in chunks of a few blocks and then files within that layer will consume space from those chunks. This kind of eliminates many of the complexities associated with space management in traditional file systems.

The global pool does not have to be locked down for various allocations happening concurrently across various layers in the file system. Another advantage being space allocated in layers will not be fragmented.

Every layer keeps track of space allocated within that layer and all that space can be returned to the global pool when the layer is deleted. Also any unused space in reserved chunks returned as well (this happens as part of sync and unmount as well).

As for shared space between layers, a layer will free space in the global pool only if the space was originally allocated in the layer, not if the space was inherited from a previous layer.

There should be a minimum size for the device to be formatted/mounted as a file system. Operations like writes, file creations and creating new layers are failed when file system free space goes below a certain threshold.

## File operations

All file operations take the shared lock on the layer the files they want to operate on or belong to. They can then proceed after taking on the locks of the files involved in those operations in the appropriate mode. For reading shared data, no lock on any inode is needed.

Unlike some other graph drivers, atomic rename is supported. Certain operations like hardlink, rename, etc., are not supported across layers of the file system (if attempted manually).

Access and creation times are not tracked.

### Writes

Writes are returned immediately after copying new data to inode page table.
Zero blocks written to files are detected. If all data written to a file is
zeros, then nothing is written to disk and such files do not consume any disk
space.

Sparse files are supported and files do not consume space on disk for sparse regions.

Writes which are not page aligned do not trigger reading at the time of write, but deferred until application reads the page again or when the page is written to disk. If the page is filled up with subsequent writes, reading of the page from disk can be completely avoided as a whole page can be written down.

### Fsync

Fsync is disabled on all files and layers are made persistent when needed. Syncing dirty pages are usually triggered on last close of a file, with the exception of files in global file system.

### rmdir

rmdir in global file system layer (layer 0) may succeed even when directory is not empty. This helps Docker daemon to delete directories no longer needed without iterating over the directory sub-tree.

### xattrs

It looks like many UNIX commands unnecessarily query or try to remove extended attributes even when the file does not have any extended attributes. When a file system does not have any files with extended attributes, these operations are failed without even looking up and locking the inode. Ideally, the kernel should avoid making these calls when the inode does not have extended attributes (that info could be cached part of previous stat calls).

### ioctls

There is support for a few ioctls for operations like creating/removing/loading/unloading layers. Currently, ioctls are supported only on the layer root directory.

## Copying up (BOW, branch-on-write)

When a shared file is modified in a layer, its metadata is copied up completely which includes the inode, whole directory or whole map depending on the type of file, all the extended attributes, etc.). Shared metadata may still be shared in cache, but separate copies are made on disk when dirty inode is flushed to disk.

Initially after a copy up of an inode, user data of file is shared between newly copied up inode and original inode and copy-up of user data happens in units of 4KB as and when user data gets modified in the layer. While copying up a page, if a whole page is modified, then old data does not have to be read-in. New data is written to a new location and old data block is untouched.

In practice, most applications truncate the whole file and write new data to the file, thus individual pages of the files are not copied up.

User data blocks and certain metadata blocks (emap blocks, directory blocks, extended attributes, etc.) are never overwritten.

## Caching

As of now, all metadata (inodes, directories, emap, extended attributes, etc.), stay in memory until the layer is unmounted or layer/file is deleted. There is no upper limit on how many of those could be cached. The actual amount of metadata is cached, not the page-aligned metadata. Almost all these are tracked using sequential lists in cache with the exception of directories bigger than a certain size which would use a hash table for tracking file names. Snapshot root directory uses a hash table always irrespective of the number of layers present.

Each layer maintains a hash table for its inodes using a hash generated by the inode number. This hash table is private to that layer.

When lookup happens on a file which is not present in a layer’s inode cache, the inode for that file is looked up traversing the parent layer chain until inode found or the base layer is reached in which case the operation is failed with ENOENT. If the operation does not require a private copy of the inode in the layer [for example, operations which simply reading data like getattr(), read(), readdir(), etc.], then the inode from the parent layer is used without instantiating another copy of the inode in the cache. If the operation involves a modification, then the inode is copied up and a new instance of the inode is instantiated in inode cache of the layer.

Each regular file inode maintains an array for dirty pages of size 4KB indexed by the page number, for recently written/modified pages, if the file was recently modified. If the file is bigger than a certain size and not a temporary file, then a hash table is used instead of the array. These pages are written out when the file is closed in read-only layers, when a file accumulate too many dirty pages, when a layer accumulate too many files with dirty pages, or when the file system is unmounted/persisted. Also each regular file inode maintains a list of extents to track emap of the file, if the file is fragmented on disk. When blocks of zeroes are written to a file, those do not create separate copies of the zeros in cache.

Blocks can be cached in chunks of size 4KB, called “pages in block cache.” Pages are cached until the layer is unmounted or the layer is deleted. This block cache has an upper limit for entries and pages are recycled when the cache hits that limit. The block cache is shared all the layers in a layer tree as data could be shared between layers in the tree. The block cache maintains a hash table using a hash generated on the block number. Pages from the cache are purged under memory pressure and/or when layers are idle for a certain time period.

As the user data is shared, multiple layers sharing the same data will use the same page in block cache, all looking up the data using the same block number. Thus there will not be multiple copies of same data in page cache. Pages cached in this private block cache is mostly shared data between layers. Data which is not shared between layers is still cached in kernel page cache.

## Data placement

Space for files is not allocated when data is written to the file, but later when dirty data is flushed to disk. This has a huge advantage that the size of the file is known at the time of space allocation and all the blocks needed for the file can be allocated as single extent if the file system is not fragmented. With the read-only layers created while populating images, files are written once and never modified and this scheme of deferred allocation helps keeping the files contiguous on disk. Also temporary files may never get written to disk (large temporary files are created for image tar files).

This also helps when applications are writing to a file randomly and/or if writes are not page aligned. Also if writes received on a file are all zeroes, those are not written to disk and therefore do not consume any space on disk.

Such a scheme also helps writing out small files after coalescing many of them together. Similarly, metadata is also placed contiguously and written out together.

As every attempt is made to place files contiguously on disk, that benefits in consuming less memory (less metadata), less disk space, and less overhead.

## I/O coalescing

When space for a file is allocated contiguously as part of flush, the dirty pages of the file can be flushed in large chunks reducing the number of I/Os issued to the device. Similarly, space for small files is allocated contiguously and their pages are written out in large chunks.

## Crash Consistency (TODO)

If the graph driver is not shutdown normally, the Docker database and layers in the graph driver need to be consistent. Also each layer needs to be consistent as well. As the graph driver manages both Docker database and images/containers, those are kept in consistent state by using checkpointing technologies. Thus this file system does not have the complexity of journaling schemes typically used in file systems to provide crash consistency.

## Layer Diff (for Docker build/commit)

Finding differences between any two layers is simply finding inodes present in layers between the old layer and new layer (inclusive). As of now, this work is pending and the graph driver is using the default NaiveDiffDriver.

## Stats

When enabled at build time, all file operations and ioctl requests are counted and times taken for each of those are tracked for each layer separately. Those stats can be queried using a command. Currently, it is also displayed at the time a layer is unmounted. Stats for a layer can be cleared before running applications to trace actual operations during any time period.

Memory usage on a per layer basis is tracked and reported as well. Similarly, count of files of different types in every layer is maintained. Also count of I/Os issued by each layer is tracked.

# Contributing

The specification and code is licensed under the Apache 2.0 license found in 
the `LICENSE` file of this repository.  

See the [Style Guide](STYLEGUIDE.md).


### Sign your work

The sign-off is a simple line at the end of the explanation for the
patch, which certifies that you wrote it or otherwise have the right to
pass it on as an open-source patch.  The rules are pretty simple: if you
can certify the below (from
[developercertificate.org](http://developercertificate.org/)):

```
Developer Certificate of Origin
Version 1.1

Copyright (C) 2004, 2006 The Linux Foundation and its contributors.
660 York Street, Suite 102,
San Francisco, CA 94110 USA

Everyone is permitted to copy and distribute verbatim copies of this
license document, but changing it is not allowed.


Developer's Certificate of Origin 1.1

By making a contribution to this project, I certify that:

(a) The contribution was created in whole or in part by me and I
    have the right to submit it under the open source license
    indicated in the file; or

(b) The contribution is based upon previous work that, to the best
    of my knowledge, is covered under an appropriate open source
    license and I have the right under that license to submit that
    work with modifications, whether created in whole or in part
    by me, under the same open source license (unless I am
    permitted to submit under a different license), as indicated
    in the file; or

(c) The contribution was provided directly to me by some other
    person who certified (a), (b) or (c) and I have not modified
    it.

(d) I understand and agree that this project and the contribution
    are public and that a record of the contribution (including all
    personal information I submit with it, including my sign-off) is
    maintained indefinitely and may be redistributed consistent with
    this project or the open source license(s) involved.
```

then you just add a line to every git commit message:

    Signed-off-by: Joe Smith <joe@gmail.com>

using your real name (sorry, no pseudonyms or anonymous contributions.)

You can add the sign off when creating the git commit via `git commit -s`.
