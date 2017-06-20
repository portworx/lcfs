#ifndef _LC_H_
#define _LC_H_

/* Maximum number of layers */
#define LC_LAYER_MAX  65535ull

/* Sessions for the mount points */
enum lc_mountId {
    LC_BASE_MOUNT = 0,  /* Mount for base file system */

    LC_LAYER_MOUNT = 1, /* Mount for layer management */

    LC_MAX_MOUNTS = 2, /* Number of mount points device mounted at */
} __attribute__((packed));

/* Time in seconds syncer is woken to checkpoint file system */
#define LC_SYNC_INTERVAL       60

/* Global file system */
struct gfs {

    /* File descriptor of the underlying device */
    int gfs_fd;

    /* Last index in use in gfs_fs/gfs_roots */
    int gfs_scount;

    /* Global File system super block */
    struct super *gfs_super;

    /* Directory inode in which layer roots are placed (/lcfs) */
    ino_t gfs_layerRoot;

    /* Inode mapping to gfs_layerRoot */
    struct inode *gfs_layerRootInode;

    /* Inode of tmp directory */
    ino_t gfs_tmp_root;

    /* Inode of local-kv.db */
    ino_t gfs_dbIno;

    /* Inode of lcfs_plugin */
    ino_t gfs_pluginIno;

    /* List of file system roots */
    ino_t *gfs_roots;

    /* List of layer file systems starting with global root fs */
    struct fs **gfs_fs;

    /* Lock protecting global list of file system chain */
    pthread_mutex_t gfs_lock;

    /* Lock used by flusher */
    pthread_mutex_t gfs_flock;

    /* Lock used by cleaner */
    pthread_mutex_t gfs_clock;

    /* Lock used by syncer */
    pthread_mutex_t gfs_slock;

    /* Thread serving base mount */
    pthread_t gfs_mountThread;

    /* Background flusher */
    pthread_t gfs_flusher;

    /* Zero page */
    char *gfs_zPage;

    /* fuse sessions */
    struct fuse_session *gfs_se[LC_MAX_MOUNTS];
#ifndef FUSE3
    /* fuse channel */
    struct fuse_chan *gfs_ch[LC_MAX_MOUNTS];
#endif

    /* Mount points */
    char *gfs_mountpoint[LC_MAX_MOUNTS];

    /* pipe to communicate with parent */
    int *gfs_waiter;

    /* Number of blocks reserved */
    uint64_t gfs_blocksReserved;

    /* Global list of extents tracking unused space */
    struct extent *gfs_extents;

    /* Extents freed from layers. Not for reuse until commit */
    struct extent *gfs_fextents;

    /* Lock protecting allocations */
    pthread_mutex_t gfs_alock;

    /* Condition to wait for mount to finish */
    pthread_cond_t gfs_mountCond;

    /* Condition on threads wait on low memory */
    pthread_cond_t gfs_mcond;

    /* Condition variable flusher thread is waiting on */
    pthread_cond_t gfs_flusherCond;

    /* Condition variable cleaner thread is waiting on */
    pthread_cond_t gfs_cleanerCond;

    /* Condition variable syncer thread is waiting on */
    pthread_cond_t gfs_syncerCond;

    /* Count of pages in use */
    uint64_t gfs_pcount;

    /* Count of total dirty pages */
    uint64_t gfs_dcount;

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

    /* Pages purged */
    uint64_t gfs_purged;

    /* Pages reused */
    uint64_t gfs_preused;

    /* Sync interval in seconds */
    int gfs_syncInterval;

    /* Count of read only layers being populated */
    int gfs_layerInProgress;

    /* Set if layers are pending flush */
    int gfs_syncRequired;

    /* Layer from pages being purged */
    int gfs_cleanerIndex;

    /* Number of mounts */
    uint8_t gfs_mcount;

    /* Set when unmount in progress */
    bool gfs_unmounting;

    /* Pages being purged */
    bool gfs_pcleaning;

    /* Set when purging of pages forced */
    bool gfs_pcleaningForced;

    /* Set if extended attributes are enabled */
    bool gfs_xattr_enabled;

    /* Set if profiling is enabled */
    bool gfs_profiling;

    /* Set if count of file types maintained */
    bool gfs_ftypes;

    /* Set if layers are swapped during commit */
    bool gfs_swapLayersForCommit;
} __attribute__((packed));

/* A file system structure created for each layer */
struct fs {

    /* File system super block */
    struct super *fs_super;

    /* Super block location */
    uint64_t fs_sblock;

    /* Index of this file system in the global table */
    int fs_gindex;

    /* Highest index used in the tree */
    int fs_hgindex;

    /* If set, invalidate pages on delete */
    int fs_pinval;

    /* Root inode of the layer */
    ino_t fs_root;

    /* Global file system */
    struct gfs *fs_gfs;

    /* Root inode */
    struct inode *fs_rootInode;

    /* Inode hash table */
    struct icache *fs_icache;

    /* Number of hash lists in icache */
    uint64_t fs_icacheSize;

    /* Page block hash table */
    struct lbcache *fs_bcache;

#ifndef LC_IC_LOCK
    /* Lock serializing inode cloning */
    pthread_mutex_t fs_ilock;
#endif

    /* Parent file system of this layer */
    struct fs *fs_parent;

    /* Root of the layer tree */
    struct fs *fs_rfs;

    /* zombie layer to be removed along with */
    struct fs *fs_zfs;

    /* Layer file system of this layer */
    struct fs *fs_child;

    /* Next file system in the layer chain of the parent fs */
    struct fs *fs_next;

    /* Previous file system in the layer chain of the parent fs */
    struct fs *fs_prev;

    /* Lock taken in shared mode by all file system operations.
     * This lock is taken in exclusive mode when layers are created/deleted.
     */
    pthread_rwlock_t fs_rwlock;

    /* Pages for writing inodes */
    struct page *fs_inodePages;

    /* Pages for writing inodes */
    struct page *fs_inodePagesLast;

    /* Pending count of inode pages to be written out */
    uint64_t fs_inodePagesCount;

    /* Current list of inode blocks */
    struct iblock *fs_inodeBlocks;

    /* Pages for writing out inode block map */
    struct page *fs_inodeBlockPages;

    /* Count of pages linked from fs_inodeBlockPages pending flushing */
    uint64_t fs_inodeBlockCount;

    /* First inode with dirty pages */
    struct inode *fs_dirtyInodes;

    /* Last inode with dirty pages */
    struct inode *fs_dirtyInodesLast;

    /* Hardlink information */
    struct hldata *fs_hlinks;

    /* Lock protecting fs_dirtyInodes list */
    pthread_mutex_t fs_dilock;

    /* Approximate size of the layer */
    uint64_t fs_size;

    /* Dirty pages pending write */
    struct page *fs_dpages;

    /* Dirty pages pending write */
    struct page *fs_dpagesLast;

    /* Time pages purged last */
    time_t fs_purgeTime;

    /* Flusher index */
    uint64_t fs_flusher;

    /* Dirty page count */
    uint64_t fs_dpcount;

    /* pcache purged last */
    uint64_t fs_purgeIndex;

    /* Lock protecting dirty page list */
    pthread_mutex_t fs_plock;

    /* Lock protecting extent lists */
    pthread_mutex_t fs_alock;

    /* Lock protecting hardlinks list */
    pthread_mutex_t fs_hlock;

    /* Changes in this layer compared to parent */
    struct cdir *fs_changes;

    /* Unused extents reserved by a layer */
    struct extent *fs_extents;

    /* Extents allocated by a layer.  Not used for root layer */
    struct extent *fs_aextents;

    /* Extents freed in layer, including inherited from parent layer */
    struct extent *fs_fextents;

#ifdef DEBUG

    /* Extents used for inodes */
    struct extent *fs_iextents;
#endif

    /* Blocks reserved */
    uint64_t fs_reservedBlocks;

    /* Stats for this file system */
    struct stats *fs_stats;

    /* Count of inodes */
    uint64_t fs_icount;

    /* Count of removed inodes */
    uint64_t fs_ricount;

    /* Count of dirty pages */
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

    /* Memory in use */
    uint64_t fs_memory;

    /* Count of allocations for each type */
    uint64_t fs_malloc[LC_MEMTYPE_MAX];

    /* Count of deallocations for each type */
    uint64_t fs_free[LC_MEMTYPE_MAX];

    /* Next index in inode block array */
    int fs_inodeIndex;

    /* Number of times layer is mounted */
    int fs_mcount;

    /* Next index in inode block */
    uint8_t fs_inodeBlockIndex;

    /* Set if superblock is dirty */
    bool fs_dirty;

    /* Set if inodes are dirty */
    bool fs_inodesDirty;

    /* Set if extents are dirty */
    bool fs_extentsDirty;

    /* Set if single read-write child of a read-only parent */
    bool fs_single;

    /* Set if readOnly layer */
    bool fs_readOnly;

    /* No more changes in the file system */
    bool fs_frozen;

    /* Set if extended attributes are enabled */
    bool fs_xattrEnabled;

    /* Set if layer is being removed */
    bool fs_removed;

    /* Set if layer is remounted */
    bool fs_restarted;

    /* Set if hlinks shared with parent */
    bool fs_sharedHlinks;

    /* Set while a layer commit is in progress */
    bool fs_commitInProgress;

    /* Set when locked exclusive */
    bool fs_locked;
} __attribute__((packed));

/* Let the syncer know something changed and a checkpoint could be triggered */
static inline void
lc_layerChanged(struct gfs *gfs, bool new, bool wakeup) {
    if (new || (gfs->gfs_syncRequired == 0)) {
        __sync_add_and_fetch(&gfs->gfs_syncRequired, 1);
    }
    if (wakeup) {
        pthread_cond_signal(&gfs->gfs_syncerCond);
    }
}

/* Mark super block dirty */
static inline void
lc_markSuperDirty(struct fs *fs) {
    if (!fs->fs_dirty) {
        fs->fs_dirty = true;
    }
}

/* Mark inodes dirty */
static inline void
lc_markInodesDirty(struct fs *fs) {
    if (!fs->fs_inodesDirty) {
        fs->fs_inodesDirty = true;
    }
}

/* Mark extents dirty */
static inline void
lc_markExtentsDirty(struct fs *fs) {
    if (!fs->fs_extentsDirty) {
        fs->fs_extentsDirty = true;
    }
}

/* Set up inode handle using inode number and file system id */
static inline uint64_t
lc_setHandle(uint64_t gindex, ino_t ino) {
    assert(gindex < LC_LAYER_MAX);
    return (gindex << LC_FH_LAYER) | ino;
}

/* Get the file system id from the file handle */
static inline uint64_t
lc_getFsHandle(uint64_t handle) {
    int gindex = handle >> LC_FH_LAYER;

    assert(gindex < LC_LAYER_MAX);
    return gindex;
}

/* Get inode number corresponding to the file handle */
static inline ino_t
lc_getInodeHandle(uint64_t handle) {
    if (handle <= LC_ROOT_INODE) {
        return LC_ROOT_INODE;
    }
    return handle & LC_FH_INODE;
}

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
