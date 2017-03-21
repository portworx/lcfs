# FUSE improvements

Provide a method to avoid unnecessary getxattr()/listxattr()/removexattr()
calls to user space when files do not have any extended attributes.  Absence of
extended attributes can be cached as part of a prior stat call or something.
https://github.com/libfuse/libfuse/issues/150

Provide a knob to bypass kernel page cache without breaking mmap(2), i.e. for
files which are not memory mapped
https://github.com/libfuse/libfuse/issues/151

Provide a knob to invalidate kernel page cache on last close
https://github.com/libfuse/libfuse/issues/152

FUSE mount ignores suid/dev mount options when specified with auto_unmount
https://github.com/libfuse/libfuse/issues/148

Separate 64 bit inode and layer (snapshot) numbers in file handle

# Docker improvements

Instead of requiring to install the graphdriver plugin from the docker hub,
it would be good to use a standalone binary as the graphdriver plugin.
This is similar to V1 plugins.  With V2 plugins, we need to download around
75MB over the network for installing the plugin.  Also /var/lib/docker needs to
be made available to two separate graphdrivers - one which installs the plugin
and then one for the plugin to operate on.  The data added to /var/lib/docker
can be completely avoided if plugin is run as a standalone binary.  That also
would help bringing up docker with lcfs much faster.

Figure out a base image to build the plugin with smaller size.  Current Centos
image used would create around 300MB of data in LCFS when installed.

Docker commit/build operations can be made more efficient if steps like generating 
diff, creating tar file and applying tar file to a new layer etc, are avoided by
simply asking the graphdriver to create a new layer with all the changes made
in the reference layer.  The tar archive created during docker commit is used for subsequent docker push/save operations.
https://github.com/docker/docker/issues/31953

Docker is leaving a file behind even after deleting an image.  That file is
recreated when the image is pulled again, so not sure why that file is left
around.

Docker is not unmounting /var/lib/docker from graphdriver plugin even after
docker is stopped.

Docker is not creating/deleting/committing layers atomically.  It updates its
database and the layers independently.  This could create orphaned layers if a
failure happens between those steps.
