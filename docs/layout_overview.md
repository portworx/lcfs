# Layout

When a new device is formatted as a new graph driver file system, a superblock with file system specific information is placed at the beginning of the device. This information identiifies this device as having a valid file system on it when it is mounted again in the future. If a device with no valid superblock is mounted, it is formatted before mounting.

Each of the layers created in the file system has a private superblock for locating data that belongs exclusively to that layer. Each layer in the file system has a unique index. This index stays the same for the lifetime of the layer.

In addition to the layers created for storing images and containers, a global file system layer keeps data that is not part of any other layer. This layer always has an index of 0. It cannot be deleted.

Superblocks of layers taken on a top of a common layer are linked together. Superblocks of the common layer point to one of these top layer superblocks. Thus superblocks of all layers on top of a layer can be reached from the superblock of that layer.

Available space is tracked using a list of free extents. There will be a single such extent immediately after the file system is formatted. The superblock of layer 0 tracks the blocks where this list is stored. Similarly, all other layers keep track of extents allocated to those layers. These blocks are also reachable from the superblock of those layers.

4KB is the smallest unit of space allocation or size of I/O to the device, called file system block size. For files larger than 4KB, multiple blocks can be allocated in a single operation. Every layer shares the whole device, and space can be allocated for any layer anywhere in the underlying device.

Each file created in any layer has an inode to track information specific to that file such as stat info, dirty data not flushed to disk, etc. Each inode has a unique identifier in the file system called its “inode number.” Files deleted in a layer do not have to maintain any whiteouts as in some union file systems since their references from the directories are removed in that layer. Inode numbers are not reused even after a file is deleted.

All UNIX file types are supported. For symbolic links, the target name is stored in the same block where inode is written. For directories, separate blocks are allocated for storing directory entries and those blocks are linked in a chain from the inode. For regular files, additional blocks are allocated for storing data and linked from the inode. When a file becomes fragmented, i.e., when an entire file cannot be stored contiguously on disk, additional blocks are allocated to track file page offsets and corresponding disk locations where data is stored, in extent format. Such blocks, called “emap blocks,” are linked from the inode as well. If the file has extended attributes, those are stored in additional blocks and linked from the inode as well. Currently, directories, emap blocks, and extended attribute blocks keep entries as linear lists.  They will be switched to better data structures like B-trees, etc., in the future as needed.

All inodes in a layer can be reached from the superblock of the layer. Every inode block is tracked in blocks linked from the superblock. Inodes are not stored in any particular order on disk. Inodes have their number within the inode.

All metadata (superblocks, inodes, directories, emap, extended attributes, etc.) are always cached in memory (although this may change in the future). They are read from disk when file system is mounted and written out when file system is unmounted.
The root directory of the file system has inode number 2 and cannot be removed. Anything created under the tmp directory in root directory is considered temporary.

### Layer root directory

There is another directory in which the roots of all layers are placed, called the “layer root directory.” This directory cannot be removed once created. It is for internal use and creating files in this directory is not allowed.

### File handles

File handles are formed by combining the layer index and the inode number of the file. This is a 64-bit number and is returned to FUSE when files are opened or created. This file handle is used to locate the file in subsequent operations such as read, readdir, write, truncate, flush, release, etc. The file handle for a shared file, when accessed from different layers, will differ because the layer index part of the file handle is different. This may turn out to be a problem when the same file is read from different layers because multiple copies of data may end up in the kernel page cache. To alleviate this problem, pages of a shared file in the kernel page cache are invalidated on its last close (this should be done when a file is closed in kernel, but FUSE does not have any knobs for doing this as of today). Also the direct-mount option cannot be used since that would prevent mmap. Ideally, FUSE should provide an option to bypass the page cache for a file if the file is not mmapped.

### Locking

Each layer has a read-write lock, which is taken in shared mode while reading or writing to the layer (all file operations). This lock is taken in exclusive mode while unmounting the root layer or while deleting any other layer.

Each inode has a read-write lock. Operations that can be run in shared mode (read, readdir, getattr, etc.), take the lock in shared mode, while other operations which modify the inode hold it in exclusive mode. This lock is not taken once a layer is frozen (meaning, a new layer is created on top of that layer and no more changes are allowed in the layer).

### Layers

New layers are added after locking the parent layer in shared mode, if there is a parent layer. The newly created layer will be linked to the parent layer. All the layers taken on a parent layer are linked together as well.

A layer with no parent layer forms a base layer. The base layer for any layer can be reached by traversing its parent layers. All layers with the same base layer form a “tree of layers”.

A layer is removed after locking it in exclusive mode. This ensures that all operations on the layer are drained. A shared lock on the base layer is also held during the operation.

The root layer is locked in shared mode while creating or deleting layers. The root layer is locked exclusively while unmounting the file system.

### Space management/reclamation

Each layer allocates space in chunks of a few blocks, then files within the layer consume space from those chunks. This eliminates many of the complexities associated with space management faced by file systems that are not designed to
support layers efficiently.

The global pool does not have to be locked down for allocations happening concurrently in different layers of the file system. Another advantage is that space allocated in layers will not be fragmented.

Every layer keeps track of space allocated within the layer and all this space is returned to the global pool when the layer is deleted. Any unused space in reserved chunks is also returned (this happens as part of sync and unmount as well).

As for shared space between layers, a layer will free space in the global pool only if the space was originally allocated in that layer, not if the space was inherited from a previous layer.

There should be a minimum size for the device to be formatted/mounted as a file system. Operations like writes, file creations and creating new layers are failed when file system free space goes below a certain threshold.
