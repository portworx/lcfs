
# Space Management

Each layer allocates space in chunks of a few blocks, then files within the layer consume space from those chunks. This eliminates many of the complexities associated with space management faced by file systems that are not designed to support layers efficiently.

## Tracking and Reclamation
The global pool does not have to be locked down for allocations happening concurrently in different layers of the file system. Another advantage is that space allocated in layers will not be fragmented.

Every layer keeps track of space allocated within the layer and all this space is returned to the global pool when the layer is deleted. Any unused space in reserved chunks is also returned (this happens as part of sync and unmount as well).

As for shared space between layers, a layer will free space in the global pool only if the space was originally allocated in that layer, not if the space was inherited from a previous layer.

There should be a minimum size for the device to be formatted/mounted as a file system. Operations like writes, file creations and creating new layers are failed when file system free space goes below a certain threshold.

## Data placement

Space for files is not allocated when data is written to the file, but later when dirty data is flushed to disk. Since the size of the file is known at the time of space allocation, all the blocks needed for the file can be allocated as single extent if the file system is not fragmented. With the read-only layers created while populating images, files are written once and never modified and this scheme of deferred allocation helps keep the files contiguous on disk. Also temporary files may never get written to disk (large temporary files are created for image tar files).

This also helps when applications are writing to a file randomly or if writes are not page aligned. Also if writes received on a file are all zeroes, those are not written to disk and therefore do not consume any space on disk.

Such a scheme also helps writing out small files after coalescing many of them together. Similarly, metadata is also placed contiguously and written out together.

Every attempt is made to place files contiguously on disk, with the benefits of consuming less memory (less metadata), less disk space, and less overhead.
## I/O coalescing

When space for a file is allocated contiguously as part of flush, the dirty pages of the file can be flushed in large chunks, reducing the number of I/Os issued to the device. Similarly, space for small files is allocated contiguously and the pages are written out in large chunks. Metadata blocks such as inode blocks, directory blocks etc, are are allocated contiguously on disk and written out in chunks.
