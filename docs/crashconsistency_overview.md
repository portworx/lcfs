# Crash Consistency

If the graph driver is not shut down normally, the Docker database and layers in the graph driver need to be consistent. Each layer needs to be consistent as well. As the graph driver manages both Docker database and images/containers, these are kept in a consistent state by checkpointing. Thus this file system does not have the complexity of journaling schemes typically used in file systems to provide crash consistency.

There is a background thread running which is responsible for creating
checkpoints of LCFS.  After an abnormal shutdown, LCFS will reset back to last
checkpoint present in the file system.  Checkpoints are taken after a layer is
created, deleted or committed.  LCFS will not overwrite data in place, except
the very first block of the file system which has block addresses where other
layers can be found.  Each superblock stores starting block addresses of its
inodes and allocated extents. So whenever any of those changed, a new version
of the superblock is created and written out.

Read-write layer created for container may be re-initialized after an abnormal
shutdown, unless the container was not committed or stopped before the crash.

Docker is not doing many operations atomically, in the sense it updates its
metadata and corresponding layers independently and if a failure happens in
between, the layers may become orphaned.  Need a tool to cleanup such
orphaned layers after an abnormal shutdown (TODO).
