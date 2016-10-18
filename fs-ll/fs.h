#ifndef _FS_H_
#define _FS_H_

#define DFS_FS_MAX  10000

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

    /* List of file system roots */
    ino_t *gfs_roots;

    /* List of layer file systems starting with global root fs */
    struct fs **gfs_fs;

    /* Lock protecting global list of file system chain */
    pthread_mutex_t gfs_lock;

    /* fuse channel */
    struct fuse_chan *gfs_ch;

    /* Last used index in gfs_fs */
    int gfs_scount;
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

    /* Index of this file system in the global table */
    int fs_gindex;
};

/* Check if specified inode belongs in global file system outside any layers */
static inline bool
dfs_globalRoot(ino_t ino) {
    return dfs_getFsHandle(ino) == 0;
}

#endif
