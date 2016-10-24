#ifndef _FS_H_
#define _FS_H_

#define DFS_FS_MAX  10000

/* Global file system */
struct gfs {

    /* File descriptor of the underlying device */
    int gfs_fd;

    /* File system super block */
    struct super *gfs_super;

    /* Directory inode in which snapshot roots are placed (/dfs) */
    ino_t gfs_snap_root;

    /* Directory inode for /tmp */
    ino_t gfs_tmp_root;

    /* Directory inode for /containers */
    ino_t gfs_containers_root;

    /* Directory inode for /image/dfs/layerdb/mounts */
    ino_t gfs_mounts_root;

    /* Directory inode for /image/dfs/layerdb/sha256 */
    ino_t gfs_sha256_root;

    /* Inode mapping to gfs_snap_root */
    struct inode *gfs_snap_rootInode;

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

    /* Set if extended attributes are enabled */
    bool gfs_xattr_enabled;
};

/* A file system structure created for each layer */
struct fs {

    /* Root inode of the layer */
    ino_t fs_root;

    /* Global file system */
    struct gfs *fs_gfs;

    /* Root inode */
    struct inode *fs_rootInode;

    /* Inode hash table */
    struct icache *fs_icache;

    /* Lock protecting layer inode chains */
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

/* Return global file system */
static inline struct fs *
dfs_getGlobalFs(struct gfs *gfs) {
    struct fs *fs = gfs->gfs_fs[0];

    assert(fs->fs_root == DFS_ROOT_INODE);
    return fs;
}

#endif
