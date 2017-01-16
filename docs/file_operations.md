# File Operations and Handles 

## File operations

All file operations take the shared lock on the layer containing the files that they want to operate on. They can then proceed after taking locks on the files involved in the operation in the appropriate mode. For reading shared data, no lock on any inode is needed.

Unlike some other graph drivers, atomic rename is supported. Certain operations like hardlink, rename, etc., are not supported across layers of the file system (if attempted manually).

Access and creation times are not tracked.

### `writes`

Writes return immediately after copying their data to inode page table.  Zero blocks written to files are detected. If all data written to a file is zeros, then nothing is written to disk and such files do not consume any disk space. If pages of a file with non-zero data are overwritten with zeroes, then corresponding blocks are freed from the file.

Sparse files are supported and files do not consume space on disk for sparse regions.

Writes that are not page-aligned do not trigger an immediate read/modify/write update but are deferred until the application reads the page again or when the page is written to disk. If later writes have filled in the rest of the pages, reading of the page from disk is completely avoided as the whole page can be written down.

### `fsync`

Fsync is disabled on all files and layers are made persistent when needed. Syncing dirty pages are usually triggered on last close of a file, with the exception of files in the global file system.
### `rmdir`

rmdir in global file system layer (layer 0) succeeds even when the directory is not empty. This helps the Docker daemon to delete unneeded directories without iterating over the directory sub-tree.

### `xattrs`

Many UNIX commands unnecessarily query or try to remove extended attributes even when the file does not have any extended attributes. When a file system does not have any files with extended attributes, these operations fail without even looking up and locking the inode. Ideally, the kernel should avoid making these calls when the inode does not have extended attributes (that info could be cached part of previous stat calls).

### `ioctls`

There is support for a few ioctls for operations like creating/removing/loading/unloading layers. Currently, ioctls are supported only on the layer root directory.

## File Handles

File handles are formed by combining the layer index and the inode number of the file. This is a 64-bit number and is returned to FUSE when files are opened or created. This file handle is used to locate the file in subsequent operations such as read, readdir, write, truncate, flush, release, etc. 

The file handle for a shared file, when accessed from different layers, will differ because the layer index part of the file handle is different. This may turn out to be a problem when the same file is read from different layers because multiple copies of data may end up in the kernel page cache. To alleviate this problem, pages of a shared file in the kernel page cache are invalidated on its last close (this should be done when a file is closed in kernel, but FUSE does not have any knobs for doing this as of today). Also the direct-mount option cannot be used since that would prevent mmap. Ideally, FUSE should provide an option to bypass the page cache for a file if the file is not mmapped.

## Locking Files
Each inode has a read-write lock. Operations that can be run in shared mode (read, readdir, getattr, etc.), take the lock in shared mode, while other operations which modify the inode hold it in exclusive mode. This lock is not taken once a layer is frozen (meaning, a new layer is created on top of that layer and no more changes are allowed in the layer).
