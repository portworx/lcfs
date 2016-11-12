#ifndef _DFS_H
#define _DFS_H

/* Supported ioctls */
enum ioctl_cmd {
    SNAP_CREATE = 101,
    CLONE_CREATE = 102,
    SNAP_REMOVE = 103,
    SNAP_MOUNT = 104,
    SNAP_UMOUNT = 105,
    SNAP_STAT = 106,
    UMOUNT_ALL = 107,
};

#endif
