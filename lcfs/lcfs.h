#ifndef _LC_H
#define _LC_H

/* Supported ioctls */
enum ioctl_cmd {
    LAYER_CREATE = 101,
    LAYER_CREATE_RW = 102,
    LAYER_REMOVE = 103,
    LAYER_MOUNT = 104,
    LAYER_UMOUNT = 105,
    LAYER_STAT = 106,
    UMOUNT_ALL = 107,
    CLEAR_STAT = 108,
};

#endif
