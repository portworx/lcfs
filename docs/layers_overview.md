
# Overview of Layers 

LCFS is a user-level filesystem, written in C, POSIX-compliant, and integrated into Linux and macOS via Fuse.  LCFS does not require any kernel modifications, making it a portable filesystem. LCFS is written with container image handling as a specific workload. 

The use of LCFS with Docker containers is similar to other filesystem-based storage drivers like AUFS and OverlayFS.  However, the LCFS filesystem operates directly on top of block devices, as opposed to over two filesystems that are then merged. Thus,  LCFS aims to directly manage at the container image’s layer level, optimize for density, and eliminate the overhead of having a second filesystem that then is merged. 

Most filesystems in use  are optimized towards persistent data, provide ACID properties for system calls and aim to work well with random read-write workloads. Alternatively, LCFS focuses on optimizing operations for the container lifecycle, including handling container image:

1. Creation
2. Cloning and launching of instances
3. Memory consumption
4. Data management - actions like deletion, forced image removal, and local system resource usage based on multiple container images being present.
5. Resource management - this includes the number of inodes reported to the kernel by way of multiple copies of the same image (or layers) running.

## Snapshots in other Drivers and in LCFS
Similar to other storage drivers, LCFS creates layers for images and read-write layers on top of them for containers.  Each image will have a base layer where the files of the image are populated initially. Additional layers are created on top of the base layer for each additional layer in the image being extracted.  Each layer shares data from the previous layer.  

If a layer modifies any data, that data is visible from the layers on top of that layer, but not from the layers below it. Also, if a container modifies data in an image, from which it is loaded, that modified data is not visible from any other derived layers on top of that image layer.

A layer in an image is a read-only snapshot sitting on top of the previous layer in the image.  Therefore, these derived layers share common data.  A layer is immutable after its contents are completely populated.  When any data inherited from a previous layer is modified while populating data to a new layer, a branch-on-write (BOW) operation is performed in increments of 4 KB blocks.  New data will be written to a newly allocated location on the back-end block storage device, and old data will no longer be accessible from the layer that modified the data (or any other layer subsequently created on top of that).  Similarly, any files deleted from a layer are not visible from that layer or on any of the layers on top of that layer.


When a read-write layer is created while starting a new container (two such layers for every container), a read-write snapshot is created on top of the image layer and mounted from the container. The container can see all the data from the image layers below the read-write layer and can create new data or modify data as needed. When data is modified, the existing data is not modified in the image layer.  Instead, a private copy with new data is made available for the read-write layer.

Traditional filesystems need to provide consistency for every system call, but for a storage driver, that is required only when a layer is created, deleted or persisted. The LCFS storage driver hosts the Docker database, with information about various images and containers.  It ensures that the database is consistent with the images and therefore the image data can be read correctly regardless of restarts or crashes.  This design eliminates the need to externally monitor or garbage inspect `/var/lib/docker`.

LCFS implements snapshots without using reference counts and thus allow an unlimited number of layers. The time to create a snapshot is independent of the size of the filesystem (devices), the size of the data set, or the number of layers present in the filesystem. Snapshots are deleted in the background and processing time depends on the amount of data actually created or modified in the snapshot. Thus creation and deletion of layers occurs instantaneously.

A layer becomes read-only after new layers are populated on top of it, and a new layer will not conflict with any modifications in progress on the parent layer.  In a storage driver, creating a new layer or removing a layer does not have to stop any in-progress operations. Thus creating or removing images and containers can proceed without any noticeable impact on other running containers.

## Layer Diff (for Docker build/commit)

Finding differences between a layer and its parent layer is simply finding differences in the sets of inodes present in layers between the old layer and new layer (inclusive).  This is done by traversing the private inode cache of the new layer and reporting any inodes instantiated in the cache along with complete path.  Directories which are modified, need to scan for changes in those compared to corresponding directories in parent layer and include all changes with complete path.  All directories in a modified path, even if those are not modified, are considered changed.  All paths to a modified file (in case of multiple links - hardlinks) need to be included as well.

Files with multiple paths to it (hardlinks), need to track all those paths in order to generate this diff correctly.  Each layer tracks parent directory inode numbers and number of links from those directories to each of those hardlinks in memory.  This is disabled for pre-existing layers and newly created child layers of those after remount.  Also this is not done for root layer.  If this diff driver cannot be used on a layer, NaiveDiffDriver is used instead.
 
## Layer Locking
Each layer has a read-write lock, which is taken in shared mode while reading or writing to the layer (all file operations). This lock is taken in exclusive mode while unmounting the root layer or while deleting any other layer.

New layers are added after locking the parent layer in shared mode, if there is a parent layer. The newly created layer will be linked to the parent layer. All the layers taken on a parent layer are linked together as well.

A layer with no parent layer forms a base layer. The base layer for any layer can be reached by traversing its parent layers. All layers with the same base layer form a “tree of layers”.

A layer is removed after locking it in exclusive mode. This ensures that all operations on the layer are drained. A shared lock on the base layer is also held during the operation.

The root layer is locked in shared mode while creating or deleting layers. The root layer is locked exclusively while unmounting the filesystem.
