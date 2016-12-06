This file system driver is implemented using fuse low level API.

*  Download this directory

    git clone git@github.com:portworx/px-graph

*  Download docker sources and add files for this driver.
   Build docker and install that. (TODO: Use docker plugin)

    git clone git@github.com:docker/docker
    
    cd docker

    mkdir daemon/graphdriver/lcfs

    cp ../px-graph/docker/daemon/graphdriver/lcfs/* daemon/graphdriver/lcfs

    cp ../px-graph/docker/daemon/graphdriver/register/lcfs* daemon/graphdriver/register

    git add --all
    
    make build && make binary
    
    sudo service docker stop
    
    sudo cp bundles/latest/binary-client/docker /usr/bin
    
    sudo cp bundles/latest/binary-daemon/dockerd /usr/bin/dockerd
    
    sudo cp bundles/latest/binary-daemon/docker-runc /usr/bin
    
    sudo cp bundles/latest/binary-daemon/docker-containerd /usr/bin
    
    sudo cp bundles/latest/binary-daemon/docker-containerd-ctr /usr/bin
    
    sudo cp bundles/latest/binary-daemon/docker-containerd-shim /usr/bin
    
    sudo service docker start

*  Install fuse library.

    Download fuse library from the following link.

    https://github.com/libfuse/libfuse/releases/download/fuse-2.9.7/fuse-2.9.7.tar.gz

    Untar it and build/install using following commands:

    ./configure

    make -j8

    make install

    If needed, export PKG_CONFIG_PATH with /usr/local/lib/pkgconfig.

*  Install tcmalloc or remove that from Makefile.

    On ubuntu, run "sudo apt-get install libgoogle-perftools-dev"

    On Centos, run "sudo yum install gperftools"

*  Build this directory by running make. (cd ../px-graph/fs; make)

*  Mount a device/file - "sudo ./lcfs 'device' 'mnt'".
   For debugging, options -f or -d could be specified.

*  Stop docker and start docker with arguments "-s lcfs -g 'mnt'".
   -g argument is needed only if 'mnt' is not /var/lib/docker.
   These options could be configured in file /etc/default/docker or
   specified as arguments like "sudo dockerd -g 'mnt' -s lcfs".

*  Run experiments, stop docker, umount - "sudo fusermount -u 'mnt'"

*  For displaying stats, run "cstat 'id' [-c]" from 'mnt'/lcfs directory.
   Make sure fuse mount is running in forground mode (-d/-f option).
   Otherwise, stats are displayed whenever a layer is unmounted.

Portworx Graphdriver

Introduction

Portworx Graphdriver is a custom built file system to meet the needs
of a docker graphdriver, similar to AUFS and Overlay graphdrivers.
Unlike AUFS and Overlay, this new graphdriver is a native file
system, which means it does not operate on top of another file system,
but operate directly on top of block devices.  Thus this file system
does not have the inefficiencies of merged file systems.

This new file system is a user level file system developed using FUSE
in C and does not require any kernel modifications, making it a portable
file system.  It is a POSIX compliant file system.
Given the ephemeral (temporary) nature of data stored in a graphdriver,
it is implemented without having some of the complexities of a general
purpose file system (for example, journal and transactions).  Most
file systems in use today are optimized towards persistent data,
provide ACID properties for system calls and attempts to work well with
random read-write workloads.

This file system is not a union file system, but uses snapshot technologies.
Similar to other graphdrivers, this graphdriver also create layers for
images and read-write layers on top of those for containers. Each image
will have a base layer, in which files of the image are populated initially.
Additional layers are created on top of the base layer for each additional
layer in the image being extracted.  Each layer shares data from the
previous layer.  If a layer modifies any data, such data is visible from the
layers on top of that layer, but not from the layers below that layer.
Also if a layer modifies some data, the original data is not visible from
any layers on top of that layer.

Each layer in the image is a read only snapshot sitting on top of the previous
layer in the image.  All layers share common data between layers.  Each of
these layers is immutable after those are populated completely.  When any
existing data, inherited from a previous layer is modified while populating
data to a new layer, a copy-on-write (COW) is performed in chunks of size 4KB.
New data will be written to newly allocated location on backend storage and
old data would no longer be accessible from the layer (or any other layer
created on top of that subsequently) which modified the data.  Similarly,
any files deleted from a layer are not visible from that layer or on any
of the layers on top of that layer.

When a read-write layer is created while starting a new container (two such
layers for every container), a read-write snapshot is created on top of the
image layer and mounted from the container.  The container can see all the
data from the image layers below the read-write layer and can create new
data or modify any existing data as needed.  As it may be clear by now,
when any existing data is modified, the data is not modified in the image
layer, but a private copy with new data is made available for the
read-write layer.

Layers are first class citizen in this graphdriver.  Everything except some
docker configuration data, are part of some layer. Traditional file systems
need to provide ACID properties for every system call, but for a graphdriver,
that is required only when a layer is created/deleted or persisted.
This graphdriver is hosting the docker database with information about
various images and containers and as long as that database is consistent
with the images and containers in the graphdriver, things would correctly.
The reason for that being any image/container can be restarted from scratch
if not present in the graphdriver.

Snapshots are implemented without using any reference counts and thus allows
supporting unlimited number of layers.  The time to create a snapshot is
independent of the size of the file system (devices), size of the data set,
or the number of layers present in the file system. Snapshots are deleted
in the background and processing time depends on the amount of data actually
created/modified in the snapshot.  Thus creation and deletion of layers
can be done instantanously.

The layers on which new layers are created are read-only after those are
populated and creating a new layer does not have to worry about stopping any
modification operations in progress on the parent layer.  That is not the case
with snapshots in traditional file systems.  In a graphdriver, creating a new
layer or removing a layer does not have to lock down anything to stop any in
progress operations and thus creating/removing images and containers can
proceed without any noticeable impact on any other running containers.

Operations within a layer is independent of the total number of layers
present in the file system.  Many snapshot implementations in other file
systems may fail on this, since snapshots are point-in-time images and
snapshots may form a chain and operations on older snapshots may be
negatively impacted.

Unlike a traditional file system, layers in a graph are deleted in the
reverse order those are created.  Newest layer is deleted first, and then the
one created just before that.  A layer in the middle of chain or the base layer
cannot be deleted when there is a newer layer on top of that layer still
around.  This simplifies the overall snapshot design since deleting a snapshot
in the middle/beginning of the chain is a lot more complex to get working.
For example, each layer easily track space allocated for storing data
created/modified by the layer and any such space can be freed without worrying
about some other layer sharing any such data.

Also layers are not for rolling back, thus it does not have to incur some of
the complexities of snapshots in a traditional file system.  There is also no
need to provide any block level differences between any two layers.

Layout

When a new device is formatted as a new graphdriver file system, a superblock
is placed with some file system specific information at the beginning of the
device.  This information helps to recognize this device to have a valid file
system on it anytime it is mounted again in the future. If a device with no
valid superblock is mounted as this file system, it is formatted before
mounting.

Similarly, each of the layers created in the file system also has a private
superblock for locating data which belongs exclusively to that layer.

Each layer in the file system has a unique index.  This index stays same for
the life time of the layer.

In addition to the layers created for storing images and containers, there
is a global file system layer which keeps data not part of any layers.  This
layer always has the index of 0.  This layer cannot be deleted.

Superblocks of layers taken on a top of a common layer are linked together.
Superblock of the common layer points to one of those top layer super blocks.
Thus superblocks of all layers taken on top of a layer can be reached from the
superblock of that common bottom layer.

Available space is tracked using a list of free extents.  There will be a
single such extent immediately after the file system is formatted.
The superblock of layer 0 tracks the blocks where this list is stored.
Similarly, all other layers keep track of extents allocated to those layers.
Those blocks are also reachable from the superblock of those layers.

4KB is the smallest unit of space allocation or size of I/O to the device,
called file system block size.  For files bigger than 4KB, multiple such
blocks can be allocated in a single operation.  Every layer share the whole
device and space can be allocated for any layer from anywhere in the
underlying device.

Each file created in any layer has an inode to track information specific to
that file like stat info, dirty data not flushed to disk etc.  Each inode has
a unique identifier in the file system called inode number.  Inodes are written
to disk and as of now, each inode takes 4KB of space on disk.  It is possible
to combine multiple inodes to a single disk block in future, as a single inode
does not need that much space on disk.  Files deleted in a layer does not have
to maintain any whiteouts like in some union file systems, as their references
from the directories are gone in that layer along with.  Inode numbers are not
reused even after a file is deleted.

All UNIX file types are supported.  For symbolic links, the target name is also
stored in the same block where inode is written.  For directories, separate blocks
are allocated for storing directory entries and those blocks are linked together
as chain and the chain is linked from the inode.  For regular files, additional
blocks are allocated for storing data and linked from the inode.  When a file
gets fragmented, i.e. when whole file data could not be stored contiguously on
disk, then additional blocks are allocated to track file page offsets and
corresponding disk locations where data is stored.  Such blocks, called bmap
blocks are linked from the inode as well.   If the file has extended attributes,
those are stored in additional blocks and linked from the inode as well.
As of now, directories, bmap blocks and extended attribute blocks are keeping
entries as a sequential list and those should be switched to use better data
structures like B-trees etc in future.

All the inodes in a layer can be reached from the superblock of the layer.
Every inode block is tracked in blocks linked from the superblock.
Inodes are not stored in any particular oder on disk and inodes have their
inode number within the inode.

All metadata (superblocks, inodes, directories, bmap, extended attributes etc) are
cached in memory always (this may change in future).  They are read from disk when
file system is mounted and written out when file system is unmounted.

The root directory of the file system has inode number 2 and cannot be removed.

Snapshot root directory

There is another directory under which roots of all layers are placed and called
snapshot root directory.  This directory also cannot be removed once created.

File handles

File handles are formed by combining layer index and inode number of the file.
This is a 64bit number and returned to FUSE when files are opened / created.
This file handle can be used to locate the same file in subsequent operations
like read, readdir, write, truncate, flush, release etc.  The file handle for a
shared file when accessed from different layers, would be different as the layer
index part of the file handle would be different.  This may turn out to be problem
when same file is read from different layers as multiple copies of data may end up
in the kernel page cache – in order to alleviate that problem, pages of a shared
file in kernel page cache are invalidated on last close of a shared file.
Also FUSE is requested to invalidate pages of a shared file on every open (this
should have been done when a file is closed, but FUSE does not have any knobs for
doing that as of today).  Also the direct mount option could not be used since that
would prevent mmap(2), ideally FUSE should provide an option to bypass pagecache
for a file when the file is not mmapped.

Locking

Each layer has a read-write lock, which is taken in shared mode while
reading/writing to the layer (all file operations).  That lock is taken in exclusive
mode while unmounting the root layer or while deleting any other layer.

Each inode has a read-write lock.  Operations which can be run in shared mode (read,
readdir, getattr etc), take that lock in shared mode, while other operations which
modify the inode hold that lock in exclusive mode.  This lock is not taken once
a layer is frozen (meaning, a new layer is created on top of that layer and no
more changes are allowed in the layer).

Layers

New layers are added after locking the parent layer in shared mode, if there
is a parent layer.  The newly created layer will be linked to the parent layer.
All the layers taken on a parent layer are linked together as well.

A layer is removed after locking that layer in exclusive mode.  That makes sure all
operations on that layer are drained.

A layer with no parent layer forms a base layer.  Base layer for any layer can be
reached by traversing the parent layers starting from that layer.  All layers with
same base layer form a “tree of layers”.

Root layer is locked in shared mode while creating/deleting layers.  Root layer
is locked exclusive while unmounting the file system.

Space management/reclamation

Each layer will allocate space in chunks of a few blocks and then files within
that layer will consume space from those chunks.  This kind of eliminates
many of the complexities associated with space management in traditional file systems.

The global pool does not have to be locked down for various allocations
happening concurrently across various layers in the file system.
Another advantage being space allocated in layers will not be fragmented.

Every layer keeps track of space allocated within that layer and all that space
can be returned to the global pool when the layer is deleted.  Also any unused
space in reserved chunks returned as well (this happens as part of sync and
unmount as well).

As for shared space between layers, a layer will free space in the global pool
only if the space was originally allocated in the layer, not if the space was
inherited from a previous layer.

There should be a minimum size for the device to be formatted/mounted as a file
system.  Operations like writes and creating new layers are failed when file
system free space goes below a certain threshold.

TODO: Handling space fragmentation due to chunk reservation

File operations

All file operations take the shared lock on the layer the files they want to
operate on belong to.  They could then proceed after taking the locks on the
files involved in those operations in the appropriate mode. For reading shared
data, no lock on any inode is needed.

Unlike some other graphdrivers, atomic rename is supported.
Certain operations like hardlink, rename etc. are not supported across layers
of the file system (if attempted manually).

Writes

Writes are returned immediately after copying new data to inode page table.

Writes which are not page aligned do not trigger reading at the time of write,
but deferred until application reads the page again or when the page is written
to disk. If the page is filled up with subsequent writes, reading of the page
from disk can be completely avoided as a whole page could be written down.

Fsync

Fsync is disabled on all files and layers are made persistent when needed.
Syncing dirty pages are usually triggered on last close of a file, with the
exception of files in global file system.

rmdir

rmdir in global file system layer (layer 0) may succeed even when directory is
not empty.  This helps docker daemon to delete directories no longer needed
without iterating over the directory sub-tree.

xattrs

It looks like many unix commands unnecessarily query or try to remove extended
attributes even when the file does not have any extended attributes.  When a
file system does not have any files with extended attributes, these operations
are failed without even looking up and locking the inode.  Ideally, the kernel
should avoid making these calls when the inode does not have extended
attributes (that info could be cached part of previous stat calls).

ioctls

There is support for a few ioctls for operations like
creating/removing/loading/unloading layers.
Currently, ioctls are supported only on the snapshot root directory.

Copying Up(COW, Redirect-On-Write)

When a shared file is modified in a layer, its metadata is copied up
completely which includes the inode, whole directory or whole bmap depending
on the type of file, all the extended attributes etc). Shared metadata may
still be shared in cache, but separate copies are made on disk when dirty inode
is flushed to disk.

Initially after a copy up of an inode, user data of file is shared between
newly copied up inode and original inode and copy-up of user data happens in
units of 4KB as and when user data gets modified in the layer.  While copying
up a page, if a whole page is modified, then old data does not have to be read-in.
New data is written to a new location and old data block is untouched.

In practice, most applications truncate the whole file and write new data to
the file, thus individual pages of the files are not copied up.

Caching

As of now, all metadata (inodes, directories, bmap, extended attributes etc),
stay in memory until the layer is unmounted or layer/file is deleted.
There is no upper limit on how many of those could be cached. Actual amount of
metadata is cached, not page aligned metadata.

Each layer maintains a hash table for its inodes using a hash generated using
the inode number.  This hash table is private to that layer.

When lookup happens on a file which is not present in a layer’s inode cache,
the inode for that file is looked up traversing the parent layer chain until
inode found or the base layer is reached in which case the operation is failed
with ENOENT.  If the operation does not require a private copy of the inode in
the layer (for example, operations which simply reading data like getattr(),
read(), readdir() etc),  then the inode from the parent layer is used without
instantiating another copy of the inode in the cache.  If the operation involves
modifying something, then the inode is copied up and a new instance of the inode
is instantiated in inode cache of the layer.

Each regular file inode maintains an array for dirty pages of size 4KB indexed
by the page number, for recently written/modified pages, if the file was
recently modified.  These pages are written out when the file is closed in
read-only layers, when a file accumulate too many dirty pages, when a layer
accumulate too many files with dirty pages or when the file system
unmounted/persisted.  Also each regular file inode maintains a
bmap table indexed by the page number, if the file is fragmented on disk.

Blocks can be cached in chunks of size 4KB, called pages in block cache.
Pages are cached until layer is unmounted or layer is deleted.  This block
cache has an upper limit for entries and pages are recycled when the cache
hits that limit.  The block cache is shared all the layers in a layer chain as
data could be shared between layers in the chain.  The block cache maintains
a hash table using a hash generated on the block number.

As the user data is shared, multiple layers sharing the same data will use the
same page in block cache, all looking up the data using the same block number.
Thus there will not be multiple copies of same data in page cache.

Data placement

Space for files is not allocated when data is written to the file, but later when
dirty data is flushed to disk.  This has a huge advantage that the size of the
file is known at the time of space allocation and all the blocks needed for the
file can be allocated as single extent if the file system is not fragmented.
With the read-only layers created while populating images, files are written
once and never modified and this scheme of deferred allocation helps keeping
the files contiguous on disk.  Also temporary files may never get written to
disk (large temporary files are created for image tar files).

This also helps when applications are writing to a file randomly and/or if
writes are not page aligned.

Such a scheme also helps writing out small files after coalescing many of those
together.  Similarly, metadata is also placed contigously and written out
together.

As every attempt is made to place files contiguously on disk, that benefits
in consuming less memory (less metadata), less disk space and less overhead.

I/O coalescing

When space for a file is allocated contiguously as part of flush,
the dirty pages of the file can be flushed in large chunks reducing the number
of I/Os issued to the device.  Similarly, space for small files is allocated
contiguously and their pages are written out in large chunks.

Crash Consistency (TODO)

If the graphdriver is not shutdown normally, the docker database and layers in
the graphdriver need to be consistent.  Also each layer needs to be consistent
as well.  As the graphdriver manages both docker database and images/containers,
those are kept in consistent state by using checkpointing technologies.  Thus
this file system does not have the complexity of journaling schemes typically
used in file systems to provide crash consistency.

Layer Diff (for docker build)

Finding differences between any two layers is simply finding inodes present in
layers between the old layer and new layer (inclusive).  As of now, this work
is pending and the graphdriver is using the default NaiveDiffDriver.

Stats

When enabled at build time, all file operations and ioctl requests are counted
and times taken for each of those are tracked for each layer separately.
Those stats can be queried using a command.  As of now, it is also displayed at
the time a layer is unmounted.  Stats for a layer can be cleared before running
applications to trace actual operations during any time period.

Memory usage on a per layer basis is tracked and reported as well.

