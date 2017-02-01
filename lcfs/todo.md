# Features

Provide a migration tool from existing graphdrivers to LCFS

Provide mapping between layer identifiers to docker images/containers

UID/GID mappings

Utility for enabling/disabling various stats

Utility for collecting logs

# LCFS improvements

Replace NaiveDiffDriver

Provide crash consistency

Eliminate the requirement for having all metadata in cache

Implement a worker queue

Replace sequential lists with better data structures (hash, B-tree etc)

Size inode and page caches based on the data set (image/container size)

Grow table of layers as new layers created rather than allocating for maximum
supported number of layers at mount time

Switch to Fuse 3.0, which has support for writeback cache and readdirplus

Reserve space for free extent map

Group free extents into buckets based on size rather than a single list

Implement read ahead

Implement better hashing scheme for better distribution in the lists

Global LRU for page cache

Replace locks with RCU

# FUSE improvements

Provide a method to avoid unnecessary getxattr()/listxattr()/removexattr()
calls to user space when files do not have any extended attributes.  Absence of
extended attributes can be cached as part of a prior stat call or something.

Provide a knob to bypass kernel page cache without breaking mmap(2), i.e. for
files which are not memory mapped

Provide a knob to invalidate kernel page cache on last close

Separate 64 bit inode and layer (snapshot) numbers in file handle
