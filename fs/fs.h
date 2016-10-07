#ifndef _FS_H_
#define _FS_H_

/* A file system structure created for each layer */
struct fs {

    /* Root inode of the layer */
    ino_t fs_root;

    /* Global file system */
    struct gfs *fs_gfs;

    /* Inodes of this layer */
    /* XXX Allocate this dynamically */
    struct inode **fs_inode;

    /* Global list of file systems */
    struct fs *fs_gnext;

    /* Parent file system of this layer */
    struct fs *fs_parent;

    /* Snapshot file system of this layer */
    struct fs *fs_snap;

    /* Next file system in the snapshot chain of the parent fs */
    struct fs *fs_next;
};

/* Global file system */
struct gfs {

    /* File descriptor of the underlying device */
    int gfs_fd;

    /* File system super block */
    struct super *gfs_super;

    /* List of layer file systems */
    struct fs *gfs_fs;

    /* Lock protecting inode chains */
    pthread_mutex_t gfs_ilock;

    /* Lock taken in shared mode by all file system operations.
     * This lock is taken in exclusive mode when snapshots are created/deleted.
     */
    pthread_rwlock_t gfs_rwlock;
};

#endif
