#ifndef _FS_H_
#define _FS_H_

/* Global file system */
struct gfs {

    /* File descriptor of the underlying device */
    int gfs_fd;

    /* File system super block */
    struct super *gfs_super;

    /* Directory inode on which snapshot roots are placed */
    ino_t gfs_snap_root;

    /* Count of inodes in use */
    ino_t gfs_ninode;

    /* List of layer file systems starting with global root fs */
    struct fs *gfs_fs;

    /* Lock protecting global list of file system chain */
    pthread_mutex_t gfs_lock;

    /* fuse channel */
    struct fuse_chan *gfs_ch;
};

/* A file system structure created for each layer */
struct fs {

    /* Root inode of the layer */
    ino_t fs_root;

    /* Global file system */
    struct gfs *fs_gfs;

    /* Inodes of this layer */
    /* XXX Allocate this dynamically */
    struct inode **fs_inode;

    /* Lock protecting inode chains */
    pthread_mutex_t *fs_ilock;

    /* Global list of file systems */
    struct fs *fs_gnext;

    /* Parent file system of this layer */
    struct fs *fs_parent;

    /* Snapshot file system of this layer */
    struct fs *fs_snap;

    /* Next file system in the snapshot chain of the parent fs */
    struct fs *fs_next;

    /* Lock taken in shared mode by all file system operations.
     * This lock is taken in exclusive mode when snapshots are created/deleted.
     */
    pthread_rwlock_t *fs_rwlock;
};

#endif
