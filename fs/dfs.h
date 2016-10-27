#ifndef _DFS_H
#define _DFS_H

/* Supported ioctls */
enum ioctl_cmd {
    SNAP_CREATE = 1,
    CLONE_CREATE = 2,
    SNAP_REMOVE = 3,
    SNAP_MOUNT = 4,
    SNAP_UMOUNT = 5,
    SNAP_STAT = 6,
    UMOUNT_ALL = 7,
};

#endif
