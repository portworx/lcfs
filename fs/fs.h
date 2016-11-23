#ifndef _LC_H_
#define _LC_H_

#define LC_MAX  4096

/* Global file system */
struct gfs {

    /* File descriptor of the underlying device */
    int gfs_fd;

    /* Global File system super block */
    struct super *gfs_super;

    /* Directory inode in which snapshot roots are placed (/lcfs) */
    ino_t gfs_snap_root;

    /* Inode mapping to gfs_snap_root */
    struct inode *gfs_snap_rootInode;

    /* List of file system roots */
    ino_t *gfs_roots;

    /* List of layer file systems starting with global root fs */
    struct fs **gfs_fs;

    /* Lock protecting global list of file system chain */
    pthread_mutex_t gfs_lock;

    /* fuse channel */
    struct fuse_chan *gfs_ch;

    /* Free extents */
    struct extent *gfs_extents;

    /* Lock protecting allocations */
    pthread_mutex_t gfs_alock;

    /* Count of pages in use */
    uint64_t gfs_pcount;

    /* Count of file systems in use */
    uint64_t gfs_count;

    /* Number of reads */
    uint64_t gfs_reads;

    /* Number of writes */
    uint64_t gfs_writes;

    /* Inodes cloned */
    uint64_t gfs_clones;

    /* Pages hit in cache */
    uint64_t gfs_phit;

    /* Pages missed in cache */
    uint64_t gfs_pmissed;

    /* Pages recycled */
    uint64_t gfs_precycle;

    /* Pages reused */
    uint64_t gfs_preused;

    /* Last index in use in gfs_fs/gfs_roots */
    int gfs_scount;

    /* Set if extended attributes are enabled */
    bool gfs_xattr_enabled;
};

/* A file system structure created for each layer */
struct fs {

    /* File system super block */
    struct super *fs_super;

    /* Super block location */
    uint64_t fs_sblock;

    /* Index of this file system in the global table */
    int fs_gindex;

    /* Root inode of the layer */
    ino_t fs_root;

    /* Global file system */
    struct gfs *fs_gfs;

    /* Root inode */
    struct inode *fs_rootInode;

    /* Inode hash table */
    struct icache *fs_icache;

    /* Page block hash table */
    struct pcache *fs_pcache;

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
    pthread_rwlock_t fs_rwlock;

    /* Pages for writing inodes */
    struct page *fs_inodePages;

    /* Pending count of inode pages to be written out */
    uint64_t fs_inodePagesCount;

    /* Current list of inode blocks */
    struct iblock *fs_inodeBlocks;

    /* Pages for writing out inode block map */
    struct page *fs_inodeBlockPages;

    /* Count of pages linked from fs_inodeBlockPages pending flushing */
    uint64_t fs_inodeBlockCount;

    /* Creation time in seconds since Epoch */
    time_t fs_ctime;

    /* Last accessed time in seconds since Epoch */
    time_t fs_atime;

    /* Dirty pages pending write */
    /* XXX Use an array? */
    struct page *fs_dpages;

    /* Dirty page count */
    uint64_t fs_dpcount;

    /* Lock protecting dirty page list */
    pthread_mutex_t fs_plock;

    /* Lock protecting extent lists */
    pthread_mutex_t fs_alock;

    /* Free extents */
    struct extent *fs_extents;

    /* Extents allocated */
    struct extent *fs_aextents;

    /* Extents to be freed */
    struct extent *fs_fextents;

    /* Metadata extents to be freed */
    struct extent *fs_mextents;

    /* Blocks currently used to store extents */
    struct extent *fs_dextents;

    /* Blocks reserved for Inodes */
    uint64_t fs_blockInodes;

    /* Count of blocks reserved for Inodes */
    uint64_t fs_blockInodesCount;

    /* Blocks reserved for metadata */
    uint64_t fs_blockMeta;

    /* Count of blocks reserved for metadata */
    uint64_t fs_blockMetaCount;

    /* Stats for this file system */
    struct stats *fs_stats;

    /* Count of inodes */
    uint64_t fs_icount;

    /* Count of pages */
    uint64_t fs_pcount;

    /* Count of blocks allocated */
    uint64_t fs_blocks;

    /* Count of blocks freed */
    uint64_t fs_freed;

    /* Number of reads */
    uint64_t fs_reads;

    /* Number of writes */
    uint64_t fs_writes;

    /* Inodes written */
    uint64_t fs_iwrite;

    /* Next index in inode block array */
    int fs_inodeIndex;

    /* Set if readOnly snapshot */
    bool fs_readOnly;

    /* Set if layer is being removed */
    bool fs_removed;
} __attribute__((packed));

/* Check if specified inode belongs in global file system outside any layers */
static inline bool
lc_globalRoot(ino_t ino) {
    return lc_getFsHandle(ino) == 0;
}

/* Return global file system */
static inline struct fs *
lc_getGlobalFs(struct gfs *gfs) {
    struct fs *fs = gfs->gfs_fs[0];

    assert(fs->fs_root == LC_ROOT_INODE);
    return fs;
}

#endif
