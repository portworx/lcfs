
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

## Layer commit

When somebody commits a container Docker creates a new image.  When a Dockerfile is built, many intermediate images are built for each command in the Dockerfile, before the final image is created.
Each command in the Dockerfile is run inside a container created of the previous image and the container is committed as an image after the command is completed.

Each time a container is committed (manually or part of building a Dockerfile), the storage driver needs to provide a list of modified files/directories in that container compared to the image it was spawned from.
Many union file systems like AUFS and Overlay(2), keep track of these changes at run time and can generate that list easily (but there are other scalability concerns).  But other storage drivers like Devicemapper and Btrfs don't keep track of changes in a container and need to generate that list by scanning for changes in the container on demand.
Docker provides a driver for this purpose, called NaiveDiffDriver.  This driver compares the underlying file systems of the container and the image, traversing the whole directory tree (namespace), looking for modified directories and files (comparing modification times, inode numbers and stuff like that).
This could be challenging for a big data set, as all the directories and files in those file systems need to be brought into the cache and numerous system calls like readdir(2), stat(2) etc. are issued during this process.

After creating the list of modified directories and files in the container, a tar archive is created, which involves reading all those modified files and then writing those to a tar file.  Again this step involves many read(2) and write(2) system calls.  Also all modified data need to transit through the page cache (which could be huge depending on the dataset).   Also the tar file may allocate space in the storage driver, and issue I/Os taking away I/O bandwidth and other resources of the system.

After the tar archive is ready, a new container is created and the data from the tar archive is extracted to the new container.  This also could turn out to be an expensive operation, as new directories and files are created, replacing any old ones around (many create/mkdir/unlink system calls) and then data copied from the tar archive using read(2) and write(2) system calls.  This data again transit through the page cache, causing duplicate instances of data and consuming a lot of system resources.  After all data populated in the new container, the tar file is removed which may require some additional work from the storage driver (free space, free inode, trim freed space etc.).

If this process was done part of building a Dockerfile, the container in which the command was run, is deleted after changes in it are committed as an image.  So in short, the whole process is simply for moving data from one container layer to an image layer.

LCFS is doing this whole process differently.  A container can be committed skipping most of the above steps.  The time for committing a container is constant irrespective of the sizes of the underlying images and amount of changes made in the container.  There is no requirement for creating a list of modified files in the container - thus there is no namespace tree traversal and all the readdir(2)/stat(2) system calls are eliminated.  There is no data movement involved as well - no need for docker to read modified data from one container layer and write to another image layer.  LCFS will take care of all that work behind the scenes, by promoting the container as an image internally from which new containers could be spawned.  The old container (which is committed now) is available as well for continued use or could be deleted.

## Layer Diff (obsolete)

Finding differences between a layer and its parent layer is simply finding differences in the sets of inodes present in layers between the old layer and new layer (inclusive).  This is done by traversing the private inode cache of the new layer and reporting any inodes instantiated in the cache along with complete path.  Directories which are modified, need to scan for changes in those compared to corresponding directories in parent layer and include all changes with complete path.  All directories in a modified path, even if those are not modified, are considered changed.  All paths to a modified file (in case of multiple links - hardlinks) need to be included as well.

Files with multiple paths to it (hardlinks), need to track all those paths in order to generate this diff correctly.  Each layer tracks parent directory inode numbers and number of links from those directories to each of those hardlinks in memory.  This is disabled for pre-existing layers and newly created child layers of those after remount.  Also this is not done for root layer.  If this diff driver cannot be used on a layer, NaiveDiffDriver is used instead.
 
## Layer Locking
Each layer has a read-write lock, which is taken in shared mode while reading or writing to the layer (all file operations). This lock is taken in exclusive mode while unmounting the root layer or while deleting any other layer.

New layers are added after locking the parent layer in shared mode, if there is a parent layer. The newly created layer will be linked to the parent layer. All the layers taken on a parent layer are linked together as well.

A layer with no parent layer forms a base layer. The base layer for any layer can be reached by traversing its parent layers. All layers with the same base layer form a “tree of layers”.

A layer is removed after locking it in exclusive mode. This ensures that all operations on the layer are drained. A shared lock on the base layer is also held during the operation.

The root layer is locked in shared mode while creating or deleting layers. The root layer is locked exclusively while unmounting the filesystem.
