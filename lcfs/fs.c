#include "includes.h"

/* Allocate a new file system structure */
struct fs *
lc_newFs(struct gfs *gfs, size_t icsize, bool rw) {
    struct fs *fs = lc_malloc(NULL, sizeof(struct fs), LC_MEMTYPE_GFS);
    time_t t;

    t = time(NULL);
    memset(fs, 0, sizeof(*fs));
    fs->fs_gfs = gfs;
    fs->fs_readOnly = !rw;
    fs->fs_ctime = t;
    fs->fs_atime = t;
    pthread_mutex_init(&fs->fs_plock, NULL);
    pthread_mutex_init(&fs->fs_dilock, NULL);
    pthread_mutex_init(&fs->fs_alock, NULL);
    pthread_rwlock_init(&fs->fs_rwlock, NULL);
    lc_icache_init(fs, icsize);
    lc_statsNew(fs);
    __sync_add_and_fetch(&gfs->gfs_count, 1);
    return fs;
}

/* Invalidate inode bmap blocks */
void
lc_invalidateInodeBlocks(struct gfs *gfs, struct fs *fs) {
    struct page *page;

    if (fs->fs_inodeBlockCount) {
        page = fs->fs_inodeBlockPages;
        fs->fs_inodeBlockPages = NULL;
        fs->fs_inodeBlockCount = 0;
        lc_releasePages(gfs, fs, page);
    }
    if (fs->fs_inodeBlocks) {
        lc_free(fs->fs_rfs, fs->fs_inodeBlocks,
                LC_BLOCK_SIZE, LC_MEMTYPE_DATA);
        fs->fs_inodeBlocks = NULL;
    }
}

/* Flush inode block map pages */
void
lc_flushInodeBlocks(struct gfs *gfs, struct fs *fs) {
    uint64_t count, block, pcount = fs->fs_inodeBlockCount;
    struct page *page, *fpage;
    struct iblock *iblock;

    if (pcount == 0) {
        return;
    }
    if (fs->fs_inodeBlocks != NULL) {
        fs->fs_inodeBlockPages = lc_getPageNoBlock(gfs, fs,
                                                   (char *)fs->fs_inodeBlocks,
                                                   fs->fs_inodeBlockPages);
        fs->fs_inodeBlocks = NULL;
    }
    block = lc_blockAllocExact(fs, pcount, true, true);
    fpage = fs->fs_inodeBlockPages;
    page = fpage;
    count = pcount;
    while (page) {
        count--;
        lc_addPageBlockHash(gfs, fs, page, block + count);
        iblock = (struct iblock *)page->p_data;
        iblock->ib_next = (page == fpage) ?
                          fs->fs_super->sb_inodeBlock : block + count + 1;
        page = page->p_dnext;
    }
    assert(count == 0);
    lc_flushPageCluster(gfs, fs, fpage, pcount);
    fs->fs_inodeBlockCount = 0;
    fs->fs_inodeBlockPages = NULL;
    fs->fs_super->sb_inodeBlock = block;
}

/* Allocate a new inode block */
void
lc_newInodeBlock(struct gfs *gfs, struct fs *fs) {
    if (fs->fs_inodeBlockCount >= LC_CLUSTER_SIZE) {
        lc_flushInodeBlocks(gfs, fs);
    }

    /* Instantiate a new page and link the inode buffer to list of inode pages
     * pending write.
     */
    if (fs->fs_inodeBlocks != NULL) {
        fs->fs_inodeBlockPages = lc_getPageNoBlock(gfs, fs,
                                                   (char *)fs->fs_inodeBlocks,
                                                   fs->fs_inodeBlockPages);
    }
    lc_mallocBlockAligned(fs->fs_rfs, (void **)&fs->fs_inodeBlocks,
                          LC_MEMTYPE_DATA);
    memset(fs->fs_inodeBlocks, 0, LC_BLOCK_SIZE);
    fs->fs_inodeIndex = 0;
    fs->fs_inodeBlockCount++;
}

/* Free resources associated with a layer */
void
lc_freeLayer(struct fs *fs, bool remove) {
    struct gfs *gfs = fs->fs_gfs;

    assert(fs->fs_blockInodesCount == 0);
    assert(fs->fs_blockMetaCount == 0);
    assert(fs->fs_dpcount == 0);
    assert(fs->fs_dpages == NULL);
    assert(fs->fs_inodePagesCount == 0);
    assert(fs->fs_inodePages == NULL);
    assert(fs->fs_inodeBlockCount == 0);
    assert(fs->fs_inodeBlockPages == NULL);
    assert(fs->fs_inodeBlocks == NULL);
    assert(fs->fs_extents == NULL);
    assert(fs->fs_aextents == NULL);
    assert(fs->fs_fextents == NULL);
    assert(fs->fs_mextents == NULL);
    assert(fs->fs_dextents == NULL);
    assert(fs->fs_icount == 0);
    assert(fs->fs_pcount == 0);
    assert(!remove || (fs->fs_blocks == fs->fs_freed));

    if (fs->fs_pcache && (fs->fs_parent == NULL)) {
        lc_destroy_pages(gfs, fs, fs->fs_pcache, remove);
    }
    if (fs->fs_ilock && (fs->fs_parent == NULL)) {
        pthread_mutex_destroy(fs->fs_ilock);
        lc_free(fs, fs->fs_ilock, sizeof(pthread_mutex_t), LC_MEMTYPE_ILOCK);
    }
    lc_statsDeinit(fs);
    pthread_mutex_destroy(&fs->fs_dilock);
    pthread_mutex_destroy(&fs->fs_plock);
    pthread_mutex_destroy(&fs->fs_alock);
    pthread_rwlock_destroy(&fs->fs_rwlock);
    __sync_sub_and_fetch(&gfs->gfs_count, 1);
    if (fs != lc_getGlobalFs(gfs)) {
        lc_free(fs, fs->fs_super, LC_BLOCK_SIZE, LC_MEMTYPE_BLOCK);
        lc_displayMemStats(fs);
        lc_checkMemStats(fs);
        lc_displayFtypeStats(fs);
        lc_free(NULL, fs, sizeof(struct fs), LC_MEMTYPE_GFS);
    }
}

/* Delete a file system */
void
lc_destroyFs(struct fs *fs, bool remove) {
    lc_destroyInodes(fs, remove);
    lc_processFreedBlocks(fs, false);
    lc_freeLayer(fs, remove);
}

/* Lock a file system in shared while starting a request.
 * File system is locked in exclusive mode while taking/deleting snapshots.
 */
void
lc_lock(struct fs *fs, bool exclusive) {
    if (exclusive) {
        pthread_rwlock_wrlock(&fs->fs_rwlock);
    } else {
        pthread_rwlock_rdlock(&fs->fs_rwlock);
    }
}

/* Trylock variant of the above */
static int
lc_tryLock(struct fs *fs, bool exclusive) {
    return exclusive ? pthread_rwlock_trywrlock(&fs->fs_rwlock) :
                       pthread_rwlock_tryrdlock(&fs->fs_rwlock);
}

/* Unlock the file system */
void
lc_unlock(struct fs *fs) {
    pthread_rwlock_unlock(&fs->fs_rwlock);
}

/* Check if the specified inode is a root of a file system and if so, return
 * the index of the new file system. Otherwise, return the index of current
 * file system.
 */
int
lc_getIndex(struct fs *nfs, ino_t parent, ino_t ino) {
    struct gfs *gfs = nfs->fs_gfs;
    int i, gindex = nfs->fs_gindex;
    ino_t root;

    /* Snapshots are allowed in one directory right now */
    if ((gindex == 0) && gfs->gfs_scount && (parent == gfs->gfs_snap_root)) {
        root = lc_getInodeHandle(ino);
        assert(lc_globalRoot(ino));
        for (i = 1; i <= gfs->gfs_scount; i++) {
            if (gfs->gfs_roots[i] == root) {
                return i;
            }
        }
    }
    return gindex;
}

/* Return the file system in which the inode belongs to */
struct fs *
lc_getfs(ino_t ino, bool exclusive) {
    int gindex = lc_getFsHandle(ino);
    struct gfs *gfs = getfs();
    struct fs *fs;

    assert(gindex < LC_MAX);
    fs = gfs->gfs_fs[gindex];
    lc_lock(fs, exclusive);
    assert(fs->fs_gindex == gindex);
    assert(gfs->gfs_roots[gindex] == fs->fs_root);
    return fs;
}

/* Lock a layer exclusive for removal, after taking it off the global
 * list.
 */
uint64_t
lc_getfsForRemoval(struct gfs *gfs, ino_t root, struct fs **fsp) {
    ino_t ino = lc_getInodeHandle(root);
    int gindex = lc_getFsHandle(root);
    struct fs *fs, *pfs, *nfs;

    assert(gindex < LC_MAX);
    pthread_mutex_lock(&gfs->gfs_lock);
    fs = gfs->gfs_fs[gindex];
    if (fs == NULL) {
        pthread_mutex_unlock(&gfs->gfs_lock);
        lc_reportError(__func__, __LINE__, root, EBUSY);
        return EBUSY;
    }
    assert(fs->fs_gindex == gindex);
    if (fs->fs_root != ino) {
        pthread_mutex_unlock(&gfs->gfs_lock);
        lc_reportError(__func__, __LINE__, root, EINVAL);
        return EINVAL;
    }
    if (fs->fs_snap) {
        pthread_mutex_unlock(&gfs->gfs_lock);
        lc_reportError(__func__, __LINE__, root, EEXIST);
        return EEXIST;
    }
    fs->fs_removed = true;
    assert(gfs->gfs_roots[gindex] == ino);
    gfs->gfs_fs[gindex] = NULL;
    gfs->gfs_roots[gindex] = 0;
    if (gfs->gfs_scount == gindex) {
        assert(gfs->gfs_scount > 0);
        gfs->gfs_scount--;
    }
    fs->fs_gindex = -1;
    pfs = fs->fs_parent;
    if (pfs && (pfs->fs_snap == fs)) {

        /* Parent points to this layer */
        pfs->fs_snap = fs->fs_next;
        if (fs->fs_next) {
            fs->fs_next->fs_prev = NULL;
        }
        pfs->fs_super->sb_childSnap = fs->fs_super->sb_nextSnap;
        pfs->fs_super->sb_flags |= LC_SUPER_DIRTY;
    } else {

        /* Remove from the common parent list */
        nfs = fs->fs_prev;
        nfs->fs_next = fs->fs_next;
        nfs->fs_super->sb_nextSnap = fs->fs_super->sb_nextSnap;
        nfs->fs_super->sb_flags |= LC_SUPER_DIRTY;
        if (fs->fs_next) {
            fs->fs_next->fs_prev = fs->fs_prev;
        }
    }
    pthread_mutex_unlock(&gfs->gfs_lock);
    lc_lock(fs, true);
    assert(fs->fs_root == ino);
    *fsp = fs;
    return 0;
}

/* Add a file system to global list of file systems */
void
lc_addfs(struct fs *fs, struct fs *pfs) {
    struct gfs *gfs = fs->fs_gfs;
    struct fs *snap;
    int i;

    fs->fs_sblock = lc_blockAllocExact(fs, 1, true, false);

    /* Find a free slot and insert the new file system */
    pthread_mutex_lock(&gfs->gfs_lock);
    for (i = 1; i < LC_MAX; i++) {
        if (gfs->gfs_fs[i] == NULL) {
            fs->fs_gindex = i;
            fs->fs_super->sb_index = i;
            gfs->gfs_fs[i] = fs;
            gfs->gfs_roots[i] = fs->fs_root;
            if (i > gfs->gfs_scount) {
                gfs->gfs_scount = i;
            }
            break;
        }
    }
    assert(i < LC_MAX);
    snap = pfs ? pfs->fs_snap : lc_getGlobalFs(gfs);

    /* Add this file system to the snapshot list or root file systems list */
    if (snap) {
        fs->fs_prev = snap;
        if (snap->fs_next) {
            snap->fs_next->fs_prev = fs;
        }
        fs->fs_next = snap->fs_next;
        snap->fs_next = fs;
        fs->fs_super->sb_nextSnap = snap->fs_super->sb_nextSnap;
        snap->fs_super->sb_nextSnap = fs->fs_sblock;
        snap->fs_super->sb_flags |= LC_SUPER_DIRTY;
    } else if (pfs) {
        pfs->fs_snap = fs;
        pfs->fs_super->sb_childSnap = fs->fs_sblock;
        pfs->fs_super->sb_flags |= LC_SUPER_DIRTY;
    }
    pthread_mutex_unlock(&gfs->gfs_lock);
}

/* Format a file system by initializing its super block */
static void
lc_format(struct gfs *gfs, struct fs *fs, size_t size) {
    lc_superInit(gfs->gfs_super, size, true);
    lc_blockAllocatorInit(gfs, fs);
    lc_rootInit(fs, fs->fs_root);
}

/* Allocate global file system */
static struct gfs *
lc_gfsAlloc(int fd) {
    struct gfs *gfs = lc_malloc(NULL, sizeof(struct gfs), LC_MEMTYPE_GFS);

    memset(gfs, 0, sizeof(struct gfs));
    gfs->gfs_fs = lc_malloc(NULL, sizeof(struct fs *) * LC_MAX,
                            LC_MEMTYPE_GFS);
    memset(gfs->gfs_fs, 0, sizeof(struct fs *) * LC_MAX);
    gfs->gfs_roots = lc_malloc(NULL, sizeof(ino_t) * LC_MAX, LC_MEMTYPE_GFS);
    memset(gfs->gfs_roots, 0, sizeof(ino_t) * LC_MAX);
    pthread_mutex_init(&gfs->gfs_lock, NULL);
    pthread_mutex_init(&gfs->gfs_alock, NULL);
    gfs->gfs_fd = fd;
    return gfs;
}

/* Initialize a file system after reading its super block */
static struct fs *
lc_initfs(struct gfs *gfs, struct fs *pfs, uint64_t block, bool child) {
    uint64_t icsize;
    struct fs *fs;
    int i;

    /* XXX Size the icache based on inodes allocated in layer */
    icsize = (child || pfs->fs_parent) ? LC_ICACHE_SIZE : LC_ICACHE_SIZE_MAX;
    fs = lc_newFs(gfs, icsize, true);
    fs->fs_sblock = block;
    lc_superRead(gfs, fs, block);
    if (fs->fs_super->sb_flags & LC_SUPER_RDWR) {
        fs->fs_readOnly = false;
    }
    fs->fs_root = fs->fs_super->sb_root;
    if (child) {

        /* First child layer of the parent */
        assert(pfs->fs_snap == NULL);
        pfs->fs_snap = fs;
        pfs->fs_frozen = true;
        fs->fs_parent = pfs;
        fs->fs_pcache = pfs->fs_pcache;
        fs->fs_pcacheSize = pfs->fs_pcacheSize;
        fs->fs_ilock = pfs->fs_ilock;
        fs->fs_rfs = pfs->fs_rfs;
    } else if (pfs->fs_parent == NULL) {

        /* Base layer */
        assert(pfs->fs_next == NULL);
        fs->fs_prev = pfs;
        pfs->fs_next = fs;
        lc_pcache_init(fs, LC_PCACHE_SIZE);
        fs->fs_ilock = lc_malloc(fs, sizeof(pthread_mutex_t),
                                 LC_MEMTYPE_ILOCK);
        pthread_mutex_init(fs->fs_ilock, NULL);
        fs->fs_rfs = fs;
    } else {

        /* Layer with common parent */
        assert(pfs->fs_next == NULL);
        fs->fs_prev = pfs;
        pfs->fs_next = fs;
        fs->fs_pcache = pfs->fs_pcache;
        fs->fs_pcacheSize = pfs->fs_pcacheSize;
        fs->fs_parent = pfs->fs_parent;
        fs->fs_ilock = pfs->fs_ilock;
        fs->fs_rfs = pfs->fs_rfs;
    }

    /* Add the layer to the global list */
    i = fs->fs_super->sb_index;
    assert(i < LC_MAX);
    assert(gfs->gfs_fs[i] == NULL);
    gfs->gfs_fs[i] = fs;
    gfs->gfs_roots[i] = fs->fs_root;
    if (i > gfs->gfs_scount) {
        gfs->gfs_scount = i;
    }
    fs->fs_gindex = i;
    lc_printf("Added fs with parent %ld root %ld index %d block %ld\n",
               fs->fs_parent ? fs->fs_parent->fs_root : - 1,
               fs->fs_root, fs->fs_gindex, block);
    return fs;
}

/* Initialize all file systems from disk */
static void
lc_initSnapshots(struct gfs *gfs, struct fs *pfs) {
    struct fs *fs, *nfs = pfs;
    uint64_t block;

    /* Initialize all snapshots of the same parent */
    block = pfs->fs_super->sb_nextSnap;
    while (block) {
        fs = lc_initfs(gfs, nfs, block, false);
        nfs = fs;
        block = fs->fs_super->sb_nextSnap;
    }

    /* Now initialize all the child snapshots */
    nfs = pfs;
    while (nfs) {
        block = nfs->fs_super->sb_childSnap;
        if (block) {
            fs = lc_initfs(gfs, nfs, block, true);
            lc_initSnapshots(gfs, fs);
        }
        nfs = nfs->fs_next;
    }
}

/* Set up some special inodes on restart */
static void
lc_setupSpecialInodes(struct gfs *gfs, struct fs *fs) {
    struct inode *dir = fs->fs_rootInode;
    ino_t ino;

    ino = lc_dirLookup(fs, dir, "tmp");
    if (ino != LC_INVALID_INODE) {
        gfs->gfs_tmp_root = ino;
        printf("tmp root %ld\n", ino);
    }
    ino = lc_dirLookup(fs, dir, "lcfs");
    if (ino != LC_INVALID_INODE) {
        gfs->gfs_snap_rootInode = lc_getInode(lc_getGlobalFs(gfs), ino,
                                               NULL, false, false);
        if (gfs->gfs_snap_rootInode) {
            lc_inodeUnlock(gfs->gfs_snap_rootInode);
        }
        gfs->gfs_snap_root = ino;
        printf("snapshot root %ld\n", ino);
    }
}

/* Mount the device */
int
lc_mount(char *device, struct gfs **gfsp) {
    struct gfs *gfs;
    struct fs *fs;
    size_t size;
    int fd, err;
    int i;

    /* Open the device for mounting */
    fd = open(device, O_RDWR | O_DIRECT | O_EXCL | O_NOATIME, 0);
    if (fd == -1) {
        perror("open");
        return errno;
    }

    /* Find the size of the device and calculate total blocks */
    size = lseek(fd, 0, SEEK_END);
    if (size == -1) {
        perror("lseek");
        return errno;
    }
    if ((size / LC_BLOCK_SIZE) < LC_MIN_BLOCKS) {
        printf("Device is too small. Minimum size required is %ld\n",
               LC_MIN_BLOCKS * LC_BLOCK_SIZE);
        return EINVAL;
    }
    gfs = lc_gfsAlloc(fd);

    /* Initialize a file system structure in memory */
    /* XXX Recreate file system after abnormal shutdown for now */
    fs = lc_newFs(gfs, LC_ICACHE_SIZE_MIN, true);
    fs->fs_root = LC_ROOT_INODE;
    fs->fs_sblock = LC_SUPER_BLOCK;
    fs->fs_rfs = fs;
    lc_pcache_init(fs, LC_PCACHE_SIZE_MIN);
    gfs->gfs_fs[0] = fs;
    gfs->gfs_roots[0] = LC_ROOT_INODE;

    /* Try to find a valid superblock, if not found, format the device */
    lc_superRead(gfs, fs, fs->fs_sblock);
    gfs->gfs_super = fs->fs_super;
    if ((gfs->gfs_super->sb_magic != LC_SUPER_MAGIC) ||
        (gfs->gfs_super->sb_version != LC_VERSION) ||
        (gfs->gfs_super->sb_flags & LC_SUPER_DIRTY)) {
        printf("Formating %s, size %ld\n", device, size);
        lc_format(gfs, fs, size);
    } else {
        if (gfs->gfs_super->sb_flags & LC_SUPER_DIRTY) {
            printf("Filesystem is dirty\n");
            return EIO;
        }
        assert(size == (gfs->gfs_super->sb_tblocks * LC_BLOCK_SIZE));
        gfs->gfs_super->sb_mounts++;
        printf("Mounting %s, size %ld nmounts %ld\n",
               device, size, gfs->gfs_super->sb_mounts);
        lc_initSnapshots(gfs, fs);
        for (i = 0; i <= gfs->gfs_scount; i++) {
            fs = gfs->gfs_fs[i];
            if (fs) {
                lc_readExtents(gfs, fs);
                err = lc_readInodes(gfs, fs);
                if (err != 0) {
                    printf("Reading inodes failed, err %d\n", err);
                    return EIO;
                }
            }
        }
        fs = lc_getGlobalFs(gfs);
        lc_setupSpecialInodes(gfs, fs);
    }

    /* Write out the file system super block */
    gfs->gfs_super->sb_flags |= LC_SUPER_DIRTY | LC_SUPER_RDWR |
                                LC_SUPER_MOUNTED;
    err = lc_superWrite(gfs, fs);
    if (err != 0) {
        printf("Superblock write failed, err %d\n", err);
    } else {
        *gfsp = gfs;
    }
    return err;
}

/* Sync a dirty file system */
void
lc_sync(struct gfs *gfs, struct fs *fs) {
    int err;

    if (fs->fs_super->sb_flags & LC_SUPER_DIRTY) {
        if (fs->fs_super->sb_flags & LC_SUPER_MOUNTED) {
            fs->fs_super->sb_flags &= ~LC_SUPER_MOUNTED;
            lc_syncInodes(gfs, fs);
            lc_flushDirtyPages(gfs, fs);
            //lc_displayAllocStats(fs);
            lc_processFreedBlocks(fs, true);
            lc_freeLayerBlocks(gfs, fs, false, false);
        }

        /* Flush everything to disk before marking file system clean */
        if (!fs->fs_removed) {
            fsync(gfs->gfs_fd);
            fs->fs_super->sb_flags &= ~LC_SUPER_DIRTY;
            err = lc_superWrite(gfs, fs);
            if (err) {
                printf("Superblock update error %d for fs index %d root %ld\n",
                       err, fs->fs_gindex, fs->fs_root);
            }
        }
    }
}

/* Sync and destroy root layer */
static void
lc_umountSync(struct gfs *gfs) {
    struct fs *fs = lc_getGlobalFs(gfs);

    lc_lock(fs, true);

    /* XXX Combine sync and destroy */
    lc_sync(gfs, fs);

    /* Release freed and unused blocks */
    lc_freeLayerBlocks(gfs, fs, true, false);

    /* Destroy all inodes.  This also releases metadata blocks of removed
     * inodes.
     */
    lc_destroyInodes(fs, false);

    /* Free allocator data structures */
    lc_blockAllocatorDeinit(gfs, fs);

    lc_freeLayer(fs, false);

    /* Finally update superblock */
    lc_superWrite(gfs, fs);
    lc_unlock(fs);
    lc_displayGlobalStats(gfs);
    gfs->gfs_super = NULL;
    lc_free(fs, fs->fs_super, LC_BLOCK_SIZE, LC_MEMTYPE_BLOCK);
    gfs->gfs_fs[0] = NULL;
    lc_displayMemStats(fs);
    lc_checkMemStats(fs);
    lc_free(NULL, fs, sizeof(struct fs), LC_MEMTYPE_GFS);
}

/* Sync dirty data from all layers */
void
lc_syncAllLayers(struct gfs *gfs) {
    struct fs *fs;
    int i;

    pthread_mutex_lock(&gfs->gfs_lock);
    for (i = 1; i <= gfs->gfs_scount; i++) {
        fs = gfs->gfs_fs[i];
        if (fs && !lc_tryLock(fs, false)) {
            pthread_mutex_unlock(&gfs->gfs_lock);
            lc_sync(gfs, fs);
            lc_unlock(fs);
            pthread_mutex_lock(&gfs->gfs_lock);
        }
    }
    pthread_mutex_unlock(&gfs->gfs_lock);
}

/* Free the global file system as part of unmount */
void
lc_unmount(struct gfs *gfs) {
    struct fs *fs;
    int i;

    lc_printf("lc_unmount: gfs_scount %d gfs_pcount %ld\n",
               gfs->gfs_scount, gfs->gfs_pcount);

    /* Flush dirty data before destroying file systems since layers may be out
     * of order in the file system table and parent layers should not be
     * destroyed before child layers as some data structures are shared.
     */
    lc_syncAllLayers(gfs);
    pthread_mutex_lock(&gfs->gfs_lock);
    for (i = 1; i <= gfs->gfs_scount; i++) {
        fs = gfs->gfs_fs[i];
        if (fs && !lc_tryLock(fs, false)) {
            gfs->gfs_fs[i] = NULL;
            pthread_mutex_unlock(&gfs->gfs_lock);
            lc_freeLayerBlocks(gfs, fs, true, false);
            lc_superWrite(gfs, fs);
            lc_unlock(fs);
            lc_destroyFs(fs, false);
            pthread_mutex_lock(&gfs->gfs_lock);
        }
    }
    pthread_mutex_unlock(&gfs->gfs_lock);
    assert(gfs->gfs_count == 1);
    lc_umountSync(gfs);
    assert(gfs->gfs_pcount == 0);
    if (gfs->gfs_fd) {
        fsync(gfs->gfs_fd);
        close(gfs->gfs_fd);
    }
    assert(gfs->gfs_count == 0);
    lc_free(NULL, gfs->gfs_fs, sizeof(struct fs *) * LC_MAX, LC_MEMTYPE_GFS);
    lc_free(NULL, gfs->gfs_roots, sizeof(ino_t) * LC_MAX, LC_MEMTYPE_GFS);
    pthread_mutex_destroy(&gfs->gfs_lock);
    pthread_mutex_destroy(&gfs->gfs_alock);
}

