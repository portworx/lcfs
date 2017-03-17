#include "includes.h"

/* Enable tracking counting of files of different types */
#ifdef LC_FTYPE_ENABLE
static bool lc_ftypeStatsEnabled = true;
#else
static bool lc_ftypeStatsEnabled = false;
#endif

/* Given an inode number, return the hash index */
/* XXX Figure out a better hashing scheme */
static inline int
lc_inodeHash(struct fs *fs, ino_t ino) {
    return ino % fs->fs_icacheSize;
}

/* Allocate and initialize inode hash table */
void
lc_icache_init(struct fs *fs, size_t size) {
    struct icache *icache = lc_malloc(fs, sizeof(struct icache) * size,
                                      LC_MEMTYPE_ICACHE);
#ifdef LC_IC_LOCK
    int i;

    for (i = 0; i < size; i++) {
        pthread_mutex_init(&icache[i].ic_lock, NULL);
        icache[i].ic_head = NULL;
        icache[i].ic_lowInode = 0;
        icache[i].ic_highInode = 0;
    }
#else
    memset(icache, 0, sizeof(struct icache) * size);
#endif
    fs->fs_icache = icache;
    fs->fs_icacheSize = size;
}

/* Copy disk inode to stat structure */
void
lc_copyStat(struct stat *st, struct inode *inode) {
    struct dinode *dinode = &inode->i_dinode;

    st->st_dev = 0;
    st->st_ino = dinode->di_ino;
    st->st_mode = dinode->di_mode;
    st->st_nlink = dinode->di_nlink;
    st->st_uid = dinode->di_uid;
    st->st_gid = dinode->di_gid;
    st->st_rdev = dinode->di_rdev;
    st->st_size = dinode->di_size;
    st->st_blksize = LC_BLOCK_SIZE;
    st->st_blocks = dinode->di_blocks;

    lc_copyStatTimes(st, dinode);
}

/* Copy stats for a file which does not exist */
void
lc_copyFakeStat(struct stat *st) {
    struct timespec tv;

    lc_gettime(&tv);
    st->st_dev = 0;
    st->st_ino = LC_COMMIT_TRIGGER_INODE;
    st->st_mode = S_IFREG | 0500;
    st->st_nlink = 1;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_rdev = 0;
    st->st_size = 0;
    st->st_blksize = LC_BLOCK_SIZE;
    st->st_blocks = 0;
    st->st_atim = tv;
    st->st_mtim = tv;
    st->st_ctim = tv;
}

/* Initialize a disk inode */
static void
lc_dinodeInit(struct inode *inode, ino_t ino, mode_t mode,
              uid_t uid, gid_t gid, dev_t rdev, size_t size, ino_t parent) {
    struct dinode *dinode = &inode->i_dinode;

    dinode->di_ino = ino;
    dinode->di_mode = mode;
    dinode->di_nlink = S_ISDIR(mode) ? 2 : 1;
    dinode->di_uid = uid;
    dinode->di_gid = gid;
    dinode->di_rdev = rdev;
    dinode->di_size = size;
    dinode->di_blocks = 0;
    dinode->di_emapdir = LC_INVALID_BLOCK;
    dinode->di_extentLength = 0;
    dinode->di_xattr = LC_INVALID_BLOCK;
    dinode->di_parent = lc_getInodeHandle(parent);
    dinode->di_private = S_ISREG(mode) ? 1 : 0;
    lc_updateInodeTimes(inode, true, true);
}

/* Allocate a new inode. Size of the inode structure is different based on the
 * type of the file.  For regular files, argument reg is set and a larger inode
 * is allocated.  Argument len is non-zero for symbolic links and memory is
 * allocated along with inode structure for holding the symbolic link target
 * name, unless the target name is shared from the parent inode.
 */
static struct inode *
lc_newInode(struct fs *fs, uint64_t len, bool reg, bool new,
            bool lock, bool block) {
    size_t size = sizeof(struct inode) + (reg ? sizeof(struct rdata) : 0);
    struct inode *inode;
    struct rdata *rdata;

    if (len) {
        size += len + 1;
    }
    inode = lc_malloc(fs, size, LC_MEMTYPE_INODE);
    inode->i_fs = fs;
    if (lock) {
        inode->i_rwlock = lc_malloc(fs, sizeof(pthread_rwlock_t),
                                    LC_MEMTYPE_IRWLOCK);
        pthread_rwlock_init(inode->i_rwlock, NULL);
    } else {
        inode->i_rwlock = NULL;
    }
    inode->i_cnext = NULL;
    inode->i_emapDirExtents = NULL;
    inode->i_xattrData = NULL;
    inode->i_ocount = 0;
    inode->i_flags = block ? LC_INODE_DISK : 0;
    inode->i_page = NULL;
    if (reg) {

        /* Initialize part of the inode allocated for regular files */
        rdata = lc_inodeGetRegData(inode);
        memset(rdata, 0, sizeof(struct rdata));
    }
    if (new) {
        __sync_add_and_fetch(&fs->fs_gfs->gfs_super->sb_inodes, 1);
    }
    __sync_add_and_fetch(&fs->fs_icount, 1);
    return inode;
}

/* Take the lock on inode in the specified mode */
void
lc_inodeLock(struct inode *inode, bool exclusive) {
    struct fs *fs;

    if (inode->i_rwlock == NULL) {
        fs = inode->i_fs;
        assert(fs->fs_frozen || fs->fs_readOnly ||
               (fs->fs_super->sb_flags & LC_SUPER_INIT));

        /* Inode locks are disabled in immutable layers */
        return;
    }
    if (exclusive) {
        pthread_rwlock_wrlock(inode->i_rwlock);
    } else {
        pthread_rwlock_rdlock(inode->i_rwlock);
    }
}

/* Unlock the inode */
void
lc_inodeUnlock(struct inode *inode) {
    struct fs *fs;

    /* i_rwlock cannot be freed while the inode is locked.  That is made sure
     * by syncing all dirty data as part of unload of a layer after locking the
     * layer exclusively, which could then be used as a parent layer.
     */
    if (inode->i_rwlock == NULL) {
        fs = inode->i_fs;
        assert(fs->fs_frozen || fs->fs_readOnly ||
               (fs->fs_super->sb_flags & LC_SUPER_INIT));
        return;
    }
    lc_lockOwned(inode->i_rwlock, false);
    pthread_rwlock_unlock(inode->i_rwlock);
}

/* Free an inode and associated resources */
static void
lc_freeInode(struct inode *inode) {
    size_t size = sizeof(struct inode);
    struct fs *fs = inode->i_fs;

    if (S_ISREG(inode->i_mode)) {

        /* Free pages of a regular file */
        lc_truncateFile(inode, 0, false);
        assert(inode->i_page == NULL);
        assert(lc_inodeGetEmap(inode) == NULL);
        assert(lc_inodeGetPageCount(inode) == 0);
        assert(lc_inodeGetDirtyPageCount(inode) == 0);
        size += sizeof(struct rdata);
    } else if (S_ISDIR(inode->i_mode)) {

        /* Free directory entries */
        lc_dirFree(inode);
    } else if (S_ISLNK(inode->i_mode)) {

        /* Free target of a symbolic link if the inode owns that */
        if (inode->i_flags & LC_INODE_SYMLINK) {
            lc_free(fs, inode->i_target, inode->i_size + 1,
                    LC_MEMTYPE_SYMLINK);
        } else if (!(inode->i_flags & LC_INODE_SHARED)) {
            size += inode->i_size + 1;
        }
        inode->i_target = NULL;
    }
    if (inode->i_xattrData) {
        lc_xattrFree(inode);
    }
    assert(inode->i_xattrData == NULL);
    if (inode->i_rwlock) {
#ifdef LC_RWLOCK_DESTROY
        pthread_rwlock_destroy(inode->i_rwlock);
#endif
        lc_free(fs, inode->i_rwlock, sizeof(pthread_rwlock_t),
                LC_MEMTYPE_IRWLOCK);
    }
    if (inode->i_emapDirExtents) {
        lc_blockFreeExtents(fs->fs_gfs, fs, inode->i_emapDirExtents, 0);
    }
    lc_free(fs, inode, size, LC_MEMTYPE_INODE);
}

/* Add an inode to the hash table of the layer */
static struct inode *
lc_addInode(struct fs *fs, struct inode *inode, int hash, bool lock,
            struct inode *new, struct inode *last) {
    ino_t ino = inode->i_ino;

    if (hash == -1) {
        hash = lc_inodeHash(fs, ino);
    }

    if (lock) {
#ifdef LC_IC_LOCK
        pthread_mutex_lock(&fs->fs_icache[hash].ic_lock);
#else
        pthread_mutex_lock(&fs->fs_ilock);
#endif
    }
    if (new) {
        if (last != fs->fs_icache[hash].ic_head) {

            /* Check if raced with another thread */
            inode = lc_lookupInodeCache(fs, ino, hash);
            if (inode) {
#ifdef LC_IC_LOCK
                pthread_mutex_unlock(&fs->fs_icache[hash].ic_lock);
#else
                pthread_mutex_unlock(&fs->fs_ilock);
#endif
                new->i_flags |= LC_INODE_SHARED;
                new->i_fs = fs;
#ifdef LC_RWLOCK_DESTROY
                lc_inodeUnlock(new);
#endif
                lc_freeInode(new);
                __sync_sub_and_fetch(&fs->fs_icount, 1);
                return inode;
            }
        }
        inode = new;
    }
#ifdef DEBUG
    assert(lc_lookupInodeCache(fs, ino, hash) == NULL);
#endif

    /* Add the inode to the hash list */
    inode->i_cnext = fs->fs_icache[hash].ic_head;
    fs->fs_icache[hash].ic_head = inode;
    if (fs->fs_icache[hash].ic_highInode < ino) {
        fs->fs_icache[hash].ic_highInode = ino;
    }
    if ((fs->fs_icache[hash].ic_lowInode == 0) ||
        (fs->fs_icache[hash].ic_lowInode > ino)) {
        fs->fs_icache[hash].ic_lowInode = ino;
    }
    if (lock) {
#ifdef LC_IC_LOCK
        pthread_mutex_unlock(&fs->fs_icache[hash].ic_lock);
#else
        pthread_mutex_unlock(&fs->fs_ilock);
#endif
    }
    return inode;
}

/* Lookup an inode in the hash list */
struct inode *
lc_lookupInodeCache(struct fs *fs, ino_t ino, int hash) {
    struct inode *inode;

    if (hash == -1) {
        hash = lc_inodeHash(fs, ino);
    }
    if ((fs->fs_icache[hash].ic_head == NULL) ||
        (ino < fs->fs_icache[hash].ic_lowInode) ||
        (ino > fs->fs_icache[hash].ic_highInode)) {
        return NULL;
    }
    /* XXX Locking not needed right now, as inodes are not removed */
    //pthread_mutex_lock(&fs->fs_icache[hash].ic_lock);
    inode = fs->fs_icache[hash].ic_head;
    while (inode && (inode->i_ino != ino)) {
        inode = inode->i_cnext;
    }
    //pthread_mutex_unlock(&fs->fs_icache[hash].ic_lock);
    return inode;
}

/* Lookup an inode in the hash list */
static struct inode *
lc_lookupInode(struct fs *fs, ino_t ino, int hash) {
    struct gfs *gfs = fs->fs_gfs;

    if (ino == fs->fs_root) {
        return fs->fs_rootInode;
    }
    if (ino == gfs->gfs_layerRoot) {
        return gfs->gfs_layerRootInode;
    }
    return lc_lookupInodeCache(fs, ino, hash);
}

/* Update inode times */
void
lc_updateInodeTimes(struct inode *inode, bool mtime, bool ctime) {
    struct timespec tv;

    assert(mtime || ctime);
    lc_gettime(&tv);
    if (mtime) {
        inode->i_dinode.di_mtime = tv;
    }
    if (ctime) {
        inode->i_dinode.di_ctime = tv;
    }
}

/* Initialize root inode of a file system */
void
lc_rootInit(struct fs *fs, ino_t root) {
    struct inode *dir = lc_newInode(fs, 0, false, false,
                                    true, false);

    lc_dinodeInit(dir, root, S_IFDIR | 0755, 0, 0, 0, 0, root);
    lc_addInode(fs, dir, -1, false, NULL, NULL);
    fs->fs_rootInode = dir;
    lc_markInodeDirty(dir, LC_INODE_DIRDIRTY);
}

/* Set up layer root inode */
void
lc_setLayerRoot(struct gfs *gfs, ino_t ino) {
    struct fs *fs = lc_getGlobalFs(gfs);;
    struct inode *dir;

    /* Switching layer root is supported just to make tests to run */
    if (gfs->gfs_layerRoot) {
        if (gfs->gfs_scount) {
            printf("Warning: Layer root changed when layers are present\n");
        }
        printf("Switching layer root from %ld to %ld\n",
               gfs->gfs_layerRoot, ino);
        gfs->gfs_layerRoot = 0;
    }
    dir = lc_getInode(fs, ino, NULL, false, true);
    if (dir) {
        gfs->gfs_layerRoot = ino;
        if (!(dir->i_flags & LC_INODE_DHASHED)) {
            lc_dirConvertHashed(fs, dir);
        }
        gfs->gfs_layerRootInode = dir;
        lc_inodeUnlock(dir);
    }
    printf("layer root inode %ld\n", ino);
}

/* Purge removed inodes from cache */
static void
lc_purgeRemovedInodes(struct gfs *gfs, struct fs *fs, char *buf) {
    uint64_t i, count = 0, rcount = 0, icacheSize = fs->fs_icacheSize;
    struct icache *icache = fs->fs_icache;
    struct inode *inode, **prev;

    for (i = 0; (i < icacheSize) && (count < fs->fs_icount); i++) {
        prev = &icache[i].ic_head;
        inode = icache[i].ic_head;
        while (inode) {
            count++;
            if (inode->i_flags & LC_INODE_REMOVED) {
                *prev = inode->i_cnext;
                lc_freeInode(inode);
                rcount++;
            } else {
                prev = &inode->i_cnext;
            }
            inode = *prev;
        }
    }
    assert(rcount == fs->fs_ricount);
    if (rcount) {
        fs->fs_ricount = 0;
        assert(fs->fs_icount >= rcount);
        fs->fs_icount -= rcount;
        lc_printf("Purged %ld removed inodes\n", rcount);
    }
}

/* Mark all inodes dirty in a layer */
static void
lc_markAllInodesDirty(struct gfs *gfs, struct fs *fs) {
    uint64_t i, count = 0, icacheSize = fs->fs_icacheSize;
    struct icache *icache = fs->fs_icache;
    struct inode *inode;

    for (i = 0; (i < icacheSize) && (count < fs->fs_icount); i++) {
        inode = icache[i].ic_head;
        while (inode) {
            count++;
            assert(!(inode->i_flags & LC_INODE_REMOVED));
            inode->i_flags &= ~LC_INODE_DISK;
            lc_markInodeDirty(inode, 0);
            inode = inode->i_cnext;
        }
    }
    fs->fs_super->sb_inodeBlock = LC_INVALID_BLOCK;
    fs->fs_super->sb_flags &= ~LC_SUPER_ICHECK;
    assert(fs->fs_inodesDirty);
    lc_layerChanged(gfs, true, false);
}

/* Read inodes from an inode block */
static bool
lc_readInodesBlock(struct gfs *gfs, struct fs *fs, uint64_t block,
                   char *buf, void *ibuf, bool lock) {
    struct inode *inode, *cinode;
    bool empty = true, reg;
    uint64_t i, len;
    off_t offset;
    ino_t ino;

    //lc_printf("Reading inodes from block %ld\n", block);
    for (i = 0; i < LC_INODE_BLOCK_MAX; i++) {
        offset = i * LC_DINODE_SIZE;
        inode = (struct inode *)&buf[offset];
        ino = inode->i_ino;
        if (ino == 0) {
            continue;
        }

        /* Check if the inode is already present in cache */
        if (fs->fs_super->sb_flags & LC_SUPER_ICHECK) {
            cinode = lc_lookupInodeCache(fs, ino, lc_inodeHash(fs, ino));
            if (cinode) {
                if (S_ISLNK(inode->i_mode) && inode->i_nlink) {
                    assert(i == 0);
                    i = LC_INODE_BLOCK_MAX;
                }
                continue;
            }
        }
        reg = S_ISREG(inode->i_mode);
        len = S_ISLNK(inode->i_mode) ? inode->i_size : 0;
        inode = lc_newInode(fs, len, reg, false, lock, true);
        memcpy(&inode->i_dinode, &buf[offset], sizeof(struct dinode));
        lc_addInode(fs, inode, -1, false, NULL, NULL);

        /* Check if this is a removed inode */
        if (inode->i_nlink == 0) {
            inode->i_flags |= LC_INODE_REMOVED;
            fs->fs_ricount++;
            continue;
        }
        empty = false;
        if (reg) {

            /* Read emap of fragmented regular files */
            lc_emapRead(gfs, fs, inode, ibuf);
        } else if (S_ISDIR(inode->i_mode)) {

            /* Read directory entries */
            lc_dirRead(gfs, fs, inode, ibuf);
        } else if (len) {
            assert(i == 0);

            /* Setup target of a symbolic link */
            inode->i_target = (((char *)inode) + sizeof(struct inode));
            memcpy(inode->i_target, &buf[offset + sizeof(struct dinode)], len);
            inode->i_target[len] = 0;
            i = LC_INODE_BLOCK_MAX;
        }

        /* Read extended attributes */
        lc_xattrRead(gfs, fs, inode, ibuf);

        /* Set up root inode when read */
        if (inode->i_ino == fs->fs_root) {
            assert(S_ISDIR(inode->i_mode));
            fs->fs_rootInode = inode;
        }
    }
    return empty;
}

/* Initialize inode table of a file system */
void
lc_readInodes(struct gfs *gfs, struct fs *fs) {
    uint64_t iblock, block = fs->fs_super->sb_inodeBlock;
    uint32_t i, j, count, iovcnt = 1, rcount;
    struct extent *extents = NULL, *extent;
    uint64_t pcount = 0, bcount = 0;
    void *ibuf = NULL, *xbuf = NULL;
    bool lock = !fs->fs_frozen;
    struct iblock *buf = NULL;
    struct iovec *iovec;

    if (block == LC_INVALID_BLOCK) {

        /* This could happen if layer crashed before unmounting */
        assert(fs->fs_gindex);
        assert(!fs->fs_frozen);
        assert(fs->fs_super->sb_flags & LC_SUPER_DIRTY);

        /* Instantiate root inode for the layer */
        lc_rootInit(fs, fs->fs_root);
        lc_cloneRootDir(fs->fs_parent->fs_rootInode, fs->fs_rootInode);
        return;
    }
    lc_printf("Reading inodes for fs %d %ld, block %ld\n", fs->fs_gindex, fs->fs_root, block);
    lc_mallocBlockAligned(fs, (void **)&buf, LC_MEMTYPE_BLOCK);
    lc_mallocBlockAligned(fs, (void **)&ibuf, LC_MEMTYPE_BLOCK);
    lc_mallocBlockAligned(fs, (void **)&xbuf, LC_MEMTYPE_BLOCK);
    iovec = alloca(LC_READ_INODE_CLUSTER_SIZE * sizeof(struct iovec));
    iovec[0].iov_base = ibuf;
    iovec[0].iov_len = LC_BLOCK_SIZE;

    /* Read inode blocks linked from the super block */
    while (block != LC_INVALID_BLOCK) {
        lc_addSpaceExtent(gfs, fs, &extents, block, 1, false);
        lc_readBlock(gfs, fs, block, buf);
        assert(buf->ib_magic == LC_INODE_MAGIC);
        lc_verifyBlock(buf, &buf->ib_crc);

        /* Process inode blocks from the block read */
        for (i = 0; i < LC_IBLOCK_MAX; i++) {
            count = buf->ib_blks[i].ie_count;
            if (count == 0) {

                /* Count blocks more than half empty */
                if (i < (LC_IBLOCK_MAX / 2)) {
                    bcount++;
                }
                break;
            }
            iblock = buf->ib_blks[i].ie_start;
            assert(iblock != LC_INVALID_BLOCK);
            lc_addSpaceExtent(gfs, fs, &extents, iblock, count, false);

            /* Initialize iovec structure */
            if (count > iovcnt) {
                for (j = iovcnt;
                     (j < count) && (j < LC_READ_INODE_CLUSTER_SIZE); j++) {
                    lc_mallocBlockAligned(fs, (void **)&iovec[j].iov_base,
                                          LC_MEMTYPE_BLOCK);
                    iovec[j].iov_len = LC_BLOCK_SIZE;
                    iovcnt++;
                }
            }

            /* Read each inode block */
            while (count) {
                j = 0;
                rcount = (count <= iovcnt) ? count : iovcnt;
                count -= rcount;
                if (rcount == 1) {
                    lc_readBlock(gfs, fs, iblock, ibuf);
                } else {
                    lc_readBlocks(gfs, fs, iovec, rcount, iblock);
                }
                while (rcount) {
                    if (lc_readInodesBlock(gfs, fs, iblock, iovec[j].iov_base,
                                           xbuf, lock)) {
                        pcount++;
                    }
                    j++;
                    iblock++;
                    rcount--;
                }
            }
        }
        block = buf->ib_next;
    }
    assert(fs->fs_rootInode != NULL);
    lc_purgeRemovedInodes(gfs, fs, ibuf);
    lc_free(fs, buf, LC_BLOCK_SIZE, LC_MEMTYPE_BLOCK);
    for (i = 0; i < iovcnt; i++) {
        lc_free(fs, iovec[i].iov_base, LC_BLOCK_SIZE, LC_MEMTYPE_BLOCK);
    }
    lc_free(fs, xbuf, LC_BLOCK_SIZE, LC_MEMTYPE_BLOCK);

    /* Rewrite inodes if some inode pages could be freed */
    if ((pcount + (bcount / 2)) > LC_INODE_RELOCATE_PCOUNT) {
        lc_printf("Rewriting inodes, pcount %ld bcount %ld\n", pcount, bcount);
        lc_markAllInodesDirty(gfs, fs);
        lc_addFreedExtents(fs, extents, true);
#ifdef DEBUG
    } else {
        extent = extents;
        fs->fs_iextents = extent;
#else
    } else while (extents) {

        /* Release extents used to track inode blocks */
        extent = extents;
        extents = extents->ex_next;
        lc_free(fs, extent, sizeof(struct extent), LC_MEMTYPE_EXTENT);
#endif
    }
}

/* Invalidate dirty inode pages */
void
lc_invalidateInodePages(struct gfs *gfs, struct fs *fs) {
    struct page *page;

    if (fs->fs_inodePagesCount) {
        page = fs->fs_inodePages;
        fs->fs_inodePages = NULL;
        fs->fs_inodePagesLast = NULL;
        fs->fs_inodePagesCount = 0;
        fs->fs_inodeBlockIndex = 0;
        lc_releasePages(gfs, fs, page, true);
    }
}

/* Zero last partial inode page */
static inline void
lc_fillupLastInodePage(struct fs *fs) {
    off_t offset;

    if (fs->fs_inodePages && (fs->fs_inodeBlockIndex < LC_INODE_BLOCK_MAX)) {
        assert(fs->fs_inodeBlockIndex);
        offset = fs->fs_inodeBlockIndex * LC_DINODE_SIZE;
        memset(&fs->fs_inodePages->p_data[offset], 0, LC_BLOCK_SIZE - offset);
    }
    fs->fs_inodeBlockIndex = 0;
}

/* Flush dirty inodes */
static void
lc_flushInodePages(struct gfs *gfs, struct fs *fs) {
    uint64_t block, count = fs->fs_inodePagesCount;
    struct page *page = fs->fs_inodePages;

    /* Zero last partial inode page */
    lc_fillupLastInodePage(fs);

    /* Allocate inode blocks */
    block = lc_blockAllocExact(fs, fs->fs_inodePagesCount, true, true);
    if ((fs->fs_inodeBlocks == NULL) || (fs->fs_inodeIndex >= LC_IBLOCK_MAX)) {
        lc_newInodeBlock(gfs, fs);
    }
    fs->fs_inodeBlocks->ib_blks[fs->fs_inodeIndex].ie_start = block;
    fs->fs_inodeBlocks->ib_blks[fs->fs_inodeIndex].ie_count = count;
    fs->fs_inodeIndex++;

    /* Insert newly allocated blocks to the list of inode blocks */
    while (count) {
        lc_addPageBlockHash(gfs, fs, page, block);
        page = page->p_dnext;
        block++;
        count--;
    }
    assert(page == NULL);
    lc_addPageForWriteBack(gfs, fs, fs->fs_inodePages, fs->fs_inodePagesLast,
                           fs->fs_inodePagesCount);
    fs->fs_inodePages = NULL;
    fs->fs_inodePagesLast = NULL;
    fs->fs_inodePagesCount = 0;
    fs->fs_inodeBlockIndex = 0;
}

/* Allocate a slot in an inode block for storing an inode */
static uint64_t
fs_allocInodeSlot(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    uint64_t offset;

    /* Start with a new block for symbolic links */
    if (S_ISLNK(inode->i_mode) && !(inode->i_flags & LC_INODE_REMOVED)) {
        lc_fillupLastInodePage(fs);
    }
    if ((fs->fs_inodeBlockIndex == 0) ||
        (fs->fs_inodeBlockIndex >= LC_INODE_BLOCK_MAX)) {
        offset = 0;
        fs->fs_inodeBlockIndex = 1;
    } else {

        /* Pick next available slot in the current inode block */
        offset = fs->fs_inodeBlockIndex * LC_DINODE_SIZE;
        fs->fs_inodeBlockIndex++;
    }

    /* Use the whole block for symbolic links */
    if (S_ISLNK(inode->i_mode) && !(inode->i_flags & LC_INODE_REMOVED)) {
        fs->fs_inodeBlockIndex = LC_INODE_BLOCK_MAX;
    }
    return offset;
}

/* Free metadata extents allocated to an inode */
static void
lc_inodeFreeMetaExtents(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    assert(inode->i_extentLength == 0);

    /* Free metadata blocks allocated to the inode */
    if (inode->i_emapDirExtents) {
        lc_addFreedExtents(fs, inode->i_emapDirExtents, false);
        inode->i_emapDirExtents = NULL;
    }
    inode->i_emapDirBlock = LC_INVALID_BLOCK;
    if (inode->i_xattrData && inode->i_xattrExtents) {
        lc_addFreedExtents(fs, inode->i_xattrExtents, false);
        inode->i_xattrExtents = NULL;
    }
    inode->i_xattrBlock = LC_INVALID_BLOCK;
}

/* Flush a dirty inode to disk */
static int
lc_flushInode(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    bool written = false;
    char *inodes;
    off_t offset;

    assert(inode->i_fs == fs);

    /* Flush extended attributes if those are modified */
    if (inode->i_flags & LC_INODE_XATTRDIRTY) {
        lc_xattrFlush(gfs, fs, inode);
    }

    if (inode->i_flags & LC_INODE_EMAPDIRTY) {

        /* Flush dirty pages and emap blocks */
        lc_emapFlush(gfs, fs, inode);
    } else if (inode->i_flags & LC_INODE_DIRDIRTY) {

        /* Flush directory entries */
        lc_dirFlush(gfs, fs, inode);
    }

    /* Write out a dirty inode */
    if (inode->i_flags & LC_INODE_DIRTY) {

        /* A removed inode with a disk copy, needs to be written out so that
         * it would be considered removed when the layer is remounted.
         */
        if (!(inode->i_flags & LC_INODE_REMOVED) ||
            (inode->i_flags & LC_INODE_DISK)) {

            /* Find an inode block with a slot for this inode */
            offset = fs_allocInodeSlot(gfs, fs, inode);
            assert(offset < LC_BLOCK_SIZE);
            written = true;
            if (inode->i_flags & LC_INODE_REMOVED) {
                assert(inode->i_nlink == 0);
                assert((inode->i_extentBlock == LC_INVALID_BLOCK) ||
                       inode->i_ocount);
                inode->i_flags &= ~LC_INODE_DISK;
            } else {
                inode->i_flags |= LC_INODE_DISK;
            }
            if (inode->i_flags & LC_INODE_DISK) {
                fs->fs_super->sb_flags |= LC_SUPER_ICHECK;
            }

            //lc_printf("Writing inode %ld to offset %ld\n", inode->i_ino, offset);
            if (offset == 0) {
                lc_mallocBlockAligned(fs->fs_rfs, (void **)&inodes,
                                      LC_MEMTYPE_DATA);
                fs->fs_inodePages = lc_getPageNoBlock(gfs, fs, (char *)inodes,
                                                      fs->fs_inodePages);
                if (fs->fs_inodePagesLast == NULL) {
                    fs->fs_inodePagesLast = fs->fs_inodePages;
                }
                fs->fs_inodePagesCount++;
            } else {
                inodes = fs->fs_inodePages->p_data;
            }
            memcpy(&inodes[offset], &inode->i_dinode, sizeof(struct dinode));

            /* Store the target of the symbolic link in the same block */
            if (S_ISLNK(inode->i_mode) &&
                !(inode->i_flags & LC_INODE_REMOVED)) {
                assert(offset == 0);
                memcpy(&inodes[sizeof(struct dinode)], inode->i_target,
                       inode->i_size);
            }
        }
        inode->i_flags &= ~LC_INODE_DIRTY;
    }
    return written ? 1 : 0;
}

/* Release inode locks as those are not needed anymore */
void
lc_freezeLayer(struct gfs *gfs, struct fs *fs) {
    uint64_t i, count = 0, rcount = 0, icsize, icacheSize = fs->fs_icacheSize;
    struct icache *icache = fs->fs_icache;
    struct inode *inode, **prev;
    bool resize;

    assert(fs->fs_readOnly || (fs->fs_super->sb_flags & LC_SUPER_INIT));
    assert(!fs->fs_frozen);
    fs->fs_size = 0;
    assert(fs->fs_ricount < fs->fs_icount);

    /* Resize icache if needed */
    fs->fs_super->sb_icount = fs->fs_icount - fs->fs_ricount;
    icsize = lc_icache_size(fs);
    resize = (icsize != icacheSize);
    if (icsize != icacheSize) {
        lc_icache_init(fs, icsize);
    }
    for (i = 0; (i < icacheSize) && (count < fs->fs_icount) && !fs->fs_removed;
         i++) {
        prev = &icache[i].ic_head;
        inode = icache[i].ic_head;
        while (inode && !fs->fs_removed) {
            count++;

            /* Removed inodes can be taken out of the cache */
            if ((inode->i_flags & LC_INODE_REMOVED) &&
                !(inode->i_flags & (LC_INODE_NOTRUNC | LC_INODE_DISK))) {
                assert(inode->i_ocount == 0);
                assert((inode->i_size == 0) || !S_ISREG(inode->i_mode));
                lc_inodeFreeMetaExtents(gfs, fs, inode);
                *prev = inode->i_cnext;
                lc_freeInode(inode);
                inode = *prev;
                rcount++;
                continue;
            }

            /* A newly committed layer may still have dirty pages */
            if (inode->i_flags & LC_INODE_EMAPDIRTY) {
                lc_flushPages(gfs, fs, inode, true, false);
            }
            assert(!S_ISREG(inode->i_mode) ||
                   (lc_inodeGetDirtyPageCount(inode) == 0));

            /* Drop locks from the inode */
#ifdef LC_RWLOCK_DESTROY
            pthread_rwlock_destroy(inode->i_rwlock);
#endif
            lc_free(fs, inode->i_rwlock, sizeof(pthread_rwlock_t),
                    LC_MEMTYPE_IRWLOCK);
            inode->i_rwlock = NULL;
            if (!(inode->i_flags & LC_INODE_REMOVED)) {
                fs->fs_size += inode->i_size;
            }
            if (resize) {
                *prev = inode->i_cnext;
                lc_addInode(fs, inode, -1, false, NULL, NULL);
            } else {
                prev = &inode->i_cnext;
            }
            inode = *prev;
        }
#ifdef LC_MUTEX_DESTROY
#ifdef LC_IC_LOCK
        if (resize) {
            pthread_mutex_destroy(&icache[i].ic_lock);
        }
#endif
#endif
    }
    assert(fs->fs_pcount == 0);
    if (rcount) {
        assert(fs->fs_icount > rcount);
        fs->fs_icount -= rcount;
        if (fs->fs_ricount != rcount) {
            assert(fs->fs_ricount > rcount);
            fs->fs_super->sb_icount += fs->fs_ricount - rcount;
        }
    }
    if (resize) {
#ifdef LC_MUTEX_DESTROY
#ifdef LC_IC_LOCK
        for (; i < icacheSize; i++) {
            pthread_mutex_destroy(&icache[i].ic_lock);
        }
#endif
#endif
        lc_free(fs, icache, sizeof(struct icache) * icacheSize,
                LC_MEMTYPE_ICACHE);
    }
}

/* Sync all dirty inodes */
void
lc_syncInodes(struct gfs *gfs, struct fs *fs, bool unmount) {
    uint64_t count = 0, icount = 0, rcount = 0, fcount = 0;
    struct inode *inode, **prev;
    int i;

    lc_printf("Syncing inodes for fs %d %ld\n", fs->fs_gindex, fs->fs_root);
    lc_markSuperDirty(fs);

    /* Start with new inode blocks */
    lc_releaseInodeBlock(gfs, fs);
    lc_fillupLastInodePage(fs);

    /* Flush the root inode first */
    inode = fs->fs_rootInode;
    if (inode && !fs->fs_removed && lc_inodeDirty(inode)) {
        count += lc_flushInode(gfs, fs, inode);
    }
    if (fs == lc_getGlobalFs(gfs)) {

        /* Flush the layer root directory */
        inode = gfs->gfs_layerRootInode;
        if (inode && !fs->fs_removed && lc_inodeDirty(inode)) {
            count += lc_flushInode(gfs, fs, inode);
        }
    }

    /* Flush rest of the dirty inodes */
    for (i = 0; (i < fs->fs_icacheSize) && (icount < fs->fs_icount) &&
                !fs->fs_removed; i++) {
        inode = fs->fs_icache[i].ic_head;
        prev = &fs->fs_icache[i].ic_head;
        while (inode && !fs->fs_removed) {
            if ((inode->i_flags & LC_INODE_REMOVED) &&
                ((inode->i_ocount == 0) || unmount)) {
                assert(lc_inodeDirty(inode));

                /* Truncate pages of a removed inode on umount */
                if (S_ISREG(inode->i_mode) && inode->i_size) {
                    lc_truncateFile(inode, 0, true);
                    inode->i_size = 0;
                }
                lc_inodeFreeMetaExtents(gfs, fs, inode);
                inode->i_flags &= ~(LC_INODE_DIRDIRTY | LC_INODE_EMAPDIRTY |
                                    LC_INODE_XATTRDIRTY);
            }
            if (lc_inodeDirty(inode)) {
                count += lc_flushInode(gfs, fs, inode);
            }
            if ((inode->i_flags & LC_INODE_REMOVED) &&
                !(inode->i_flags & LC_INODE_NOTRUNC) &&
                (inode->i_ocount == 0)) {

                /* Purge removed inodes */
                *prev = inode->i_cnext;
                lc_freeInode(inode);
                rcount++;
            } else if (unmount) {
                *prev = inode->i_cnext;
                lc_freeInode(inode);
                fcount++;
            } else {
                prev = &inode->i_cnext;
            }
            icount++;
            inode = *prev;
        }
    }
    if (rcount) {
        assert(fs->fs_ricount >= rcount);
        fs->fs_ricount -= rcount;
        assert(fs->fs_icount > rcount);
        fs->fs_icount -= rcount;
    }
    if (unmount) {
        assert(fs->fs_icount == fcount);
        fs->fs_icount = 0;
    }
    if (fs->fs_inodePagesCount && !fs->fs_removed) {
        lc_flushInodePages(gfs, fs);
    }
    if (!fs->fs_removed) {
        lc_flushInodeBlocks(gfs, fs);
    }
    if (count) {
        __sync_add_and_fetch(&fs->fs_iwrite, count);
    }
}

/* Invalidate pages in kernel page cache for the layer */
void
lc_invalidateLayerPages(struct gfs *gfs, struct fs *fs) {
    uint64_t i, count = 0;
    struct inode *inode;

    for (i = 0;
         (i < fs->fs_icacheSize) && (count < fs->fs_icount) && !fs->fs_removed;
         i++) {
        inode = fs->fs_icache[i].ic_head;
        while (inode && !fs->fs_removed) {
            if (S_ISREG(inode->i_mode) && !inode->i_private && inode->i_size) {
                lc_invalInodePages(gfs, inode->i_ino);
            }
            count++;
            inode = inode->i_cnext;
        }
    }
}

/* Destroy inodes belong to a file system */
void
lc_destroyInodes(struct fs *fs, bool remove) {
    uint64_t icount = 0, rcount = 0;
    struct gfs *gfs = fs->fs_gfs;
    struct inode *inode;
    int i;

    /* Take the inode off the hash list */
    for (i = 0; (i < fs->fs_icacheSize) && (icount < fs->fs_icount); i++) {
        /* XXX Lock is not needed as the file system is locked for exclusive
         * access
         * */
        //pthread_mutex_lock(&fs->fs_icache[i].ic_lock);
        while ((inode = fs->fs_icache[i].ic_head)) {
            fs->fs_icache[i].ic_head = inode->i_cnext;
            if (!(inode->i_flags & LC_INODE_REMOVED)) {
                rcount++;
            }

            /* Invalidate kernel page cache when a layer is deleted */
            if (remove && !fs->fs_readOnly && inode->i_private &&
                inode->i_size) {
                lc_invalInodePages(gfs, inode->i_ino);
            }
            lc_freeInode(inode);
            icount++;
        }
        assert(fs->fs_icache[i].ic_head == NULL);
        //pthread_mutex_unlock(&fs->fs_icache[i].ic_lock);
#ifdef LC_MUTEX_DESTROY
#ifdef LC_IC_LOCK
        pthread_mutex_destroy(&fs->fs_icache[i].ic_lock);
#endif
#endif
    }

    /* XXX reuse this cache for another file system */
    lc_free(fs, fs->fs_icache, sizeof(struct icache) * fs->fs_icacheSize,
            LC_MEMTYPE_ICACHE);
    if (remove && icount) {
        __sync_sub_and_fetch(&gfs->gfs_super->sb_inodes, rcount);
    }
    assert(fs->fs_icount == icount);
    fs->fs_icount = 0;
}

/* Clone the root directory from parent */
void
lc_cloneRootDir(struct inode *pdir, struct inode *dir) {
    dir->i_size = pdir->i_size;
    dir->i_nlink = pdir->i_nlink;
    dir->i_dirent = pdir->i_dirent;
    if (pdir->i_flags & LC_INODE_DHASHED) {
        dir->i_flags |= LC_INODE_DHASHED | LC_INODE_SHARED;
    } else {
        dir->i_flags |= LC_INODE_SHARED;
    }
}

/* Clone an inode from a parent layer */
struct inode *
lc_cloneInode(struct fs *fs, struct inode *parent, ino_t ino, int hash,
              struct inode *last, bool exclusive) {
    bool reg = S_ISREG(parent->i_mode);
    struct inode *inode, *new;
    int flags = 0;

    assert(fs->fs_child == NULL);

    /* Initialize the inode and add to the hash and drop the layer lock after
     * taking the lock on the inode.
     */
    new = lc_newInode(fs, 0, reg, false, true, false);
    memcpy(&new->i_dinode, &parent->i_dinode, sizeof(struct dinode));
    lc_inodeLock(new, true);
    inode = lc_addInode(fs, new, hash, true, new, last);
    if (inode != new) {
        lc_inodeLock(inode, exclusive);
        return inode;
    }
    inode->i_private = 0;
    if (reg) {
        assert(parent->i_page == NULL);
        assert(lc_inodeGetDirtyPageCount(parent) == 0);

        /* Share emap and blocks initially */
        if (parent->i_dinode.di_blocks) {
            if (parent->i_extentLength) {
                inode->i_extentBlock = parent->i_extentBlock;
                inode->i_extentLength = parent->i_extentLength;
            } else {
                assert(lc_inodeGetEmap(parent));
                lc_inodeSetEmap(inode, lc_inodeGetEmap(parent));
                inode->i_flags |= LC_INODE_SHARED;
                flags |= LC_INODE_EMAPDIRTY;
            }
            flags |= LC_INODE_NOTRUNC;
        } else {

            /* A file with no blocks is not sharing anything with parent */
            inode->i_private = 1;
        }
    } else if (S_ISDIR(inode->i_mode)) {
        if (parent->i_dirent) {

            /* Directory entries are shared initially */
            inode->i_dirent = parent->i_dirent;
            inode->i_flags |= LC_INODE_SHARED;
            if (parent->i_flags & LC_INODE_DHASHED) {
                inode->i_flags |= LC_INODE_DHASHED;
            }
            flags |= LC_INODE_DIRDIRTY;
        } else {
            assert(parent->i_size == 0);
        }
    } else if (S_ISLNK(inode->i_mode)) {

        /* Target of symbolic link is shared with parent */
        inode->i_target = parent->i_target;
        inode->i_flags |= LC_INODE_SHARED;
    }

    /* Parent is different for files in root directory */
    inode->i_parent = (parent->i_parent == parent->i_fs->fs_root) ?
                      fs->fs_root : parent->i_parent;
    if (parent->i_flags & LC_INODE_MLINKS) {
        inode->i_flags |= LC_INODE_MLINKS;
    }
    if (lc_xattrCopy(inode, parent)) {
        flags |= LC_INODE_XATTRDIRTY;
    }
    lc_markInodeDirty(inode, flags);

    /* If shared lock is requested, take that after dropping exclusive lock */
    if (!exclusive) {
        lc_inodeUnlock(inode);
        lc_inodeLock(inode, false);
    }
    __sync_add_and_fetch(&fs->fs_gfs->gfs_clones, 1);
    lc_updateFtypeStats(fs, inode->i_mode, true);
    return inode;
}

/* Lookup the requested inode in the parent chain.  Inode is locked only if
 * cloned to the layer
 */
static struct inode *
lc_getInodeParent(struct fs *fs, ino_t inum, int fhash, struct inode *last,
                  bool copy, bool exclusive) {
    struct inode *inode = NULL, *parent;
    uint64_t csize = 0;
    struct fs *pfs;
    int hash = -1;

    pfs = fs->fs_parent;
    while (pfs) {
        assert(inum != pfs->fs_root);
        assert(pfs->fs_frozen || pfs->fs_commitInProgress);

        /* Hash changes with inode cache size */
        if (pfs->fs_icacheSize != csize) {
            hash = lc_inodeHash(pfs, inum);
            csize = pfs->fs_icacheSize;
        }

        /* Check parent layers until an inode is found */
        parent = lc_lookupInodeCache(pfs, inum, hash);
        if (parent != NULL) {
            assert(!(parent->i_flags & LC_INODE_REMOVED));
            if (copy) {

                /* Clone the inode only when modified */
                inode = lc_cloneInode(fs, parent, inum, fhash, last,
                                      exclusive);
            } else {

                /* XXX Remember this for future lookup */
                inode = parent;
            }
            break;
        }
        pfs = pfs->fs_parent;
    }
    return inode;
}

/* Get an inode locked in the requested mode */
struct inode *
lc_getInode(struct fs *fs, ino_t ino, struct inode *handle,
            bool copy, bool exclusive) {
    ino_t inum = lc_getInodeHandle(ino);
    struct inode *inode, *last;
    int hash;

    assert(!fs->fs_removed);
    lc_lockOwned(&fs->fs_rwlock, false);

    /* Check if the file handle points to the inode */
    if (handle && (handle->i_fs == fs)) {
        inode = handle;
        assert(inode->i_ino == inum);
        lc_inodeLock(inode, exclusive);
        return inode;
    }

    /* Check if the file system has the inode or not */
    hash = lc_inodeHash(fs, inum);
    last = fs->fs_icache[hash].ic_head;
    inode = lc_lookupInode(fs, inum, hash);
    if (inode) {
        lc_inodeLock(inode, exclusive);
        return inode;
    }

    /* If just reading inode and parent inode is known, return that */
    if (handle && !copy) {
        inode = handle;
        assert(inode->i_ino == inum);
        assert(inode->i_fs->fs_rfs == fs->fs_rfs);
        lc_inodeLock(inode, exclusive);
        return inode;
    }

    /* Lookup inode in the parent chain */
    if (fs->fs_parent) {
        inode = lc_getInodeParent(fs, inum, hash, last, copy, exclusive);
    }
    lc_lockOwned(inode->i_rwlock, exclusive);
    assert(!copy || (inode->i_fs == fs));
    return inode;
}

/* Allocate a new inode */
ino_t
lc_inodeAlloc(struct fs *fs) {
    return __sync_add_and_fetch(&fs->fs_gfs->gfs_super->sb_ninode, 1);
}

/* Update file type counts in super block */
void
lc_updateFtypeStats(struct fs *fs, mode_t mode, bool incr) {
    enum lc_ftypes ftype, count;

    if (!lc_ftypeStatsEnabled) {
        return;
    }
    if (S_ISREG(mode)) {
        ftype = LC_FTYPE_REGULAR;
    } else if (S_ISDIR(mode)) {
        ftype = LC_FTYPE_DIRECTORY;
    } else if (S_ISLNK(mode)) {
        ftype = LC_FTYPE_SYMBOLIC_LINK;
    } else {
        ftype = LC_FTYPE_OTHER;
    }
    if (incr) {
        __sync_add_and_fetch(&fs->fs_super->sb_ftypes[ftype], 1);
    } else {
        count = __sync_fetch_and_sub(&fs->fs_super->sb_ftypes[ftype], 1);
        assert(count > 0);
    }
}

/* Display file type stats for the layer */
void
lc_displayFtypeStats(struct fs *fs) {
    struct super *super;

    if (!lc_ftypeStatsEnabled) {
        return;
    }
    super = fs->fs_super;
    printf("\tRegular files %ld Directories %ld Symbolic links %ld Other %ld\n",
           super->sb_ftypes[LC_FTYPE_REGULAR],
           super->sb_ftypes[LC_FTYPE_DIRECTORY],
           super->sb_ftypes[LC_FTYPE_SYMBOLIC_LINK],
           super->sb_ftypes[LC_FTYPE_OTHER]);
}

/* Initialize a newly allocated inode */
struct inode *
lc_inodeInit(struct fs *fs, mode_t mode, uid_t uid, gid_t gid,
             dev_t rdev, ino_t parent, const char *target) {
    int len = (target != NULL) ? strlen(target) : 0;
    struct inode *inode;

    inode = lc_newInode(fs, len, S_ISREG(mode), true, true, false);
    if (len) {

        /* Copy the target of symbolic link */
        inode->i_target = (((char *)inode) + sizeof(struct inode));
        memcpy(inode->i_target, target, len);
        inode->i_target[len] = 0;
    }
    lc_dinodeInit(inode, lc_inodeAlloc(fs), mode, uid, gid, rdev, len, parent);
    lc_updateFtypeStats(fs, mode, true);
    lc_addInode(fs, inode, -1, true, NULL, NULL);
    lc_inodeLock(inode, true);
    return inode;
}

/* Move inodes from one layer to another */
void
lc_moveInodes(struct fs *fs, struct fs *cfs) {
    uint64_t i, count = 0, mcount = 0, icount = fs->fs_icount;
    struct inode *inode, *pinode, *dir, **prev;
    struct dirent *dirent;
    ino_t parent;
    size_t size;

    for (i = 0; (i < fs->fs_icacheSize) && (count < icount); i++) {
        pinode = fs->fs_icache[i].ic_head;
        prev = &fs->fs_icache[i].ic_head;
        while (pinode) {
            count++;
            assert(pinode->i_dinode.di_blocks == 0);
            assert(!(pinode->i_flags & LC_INODE_DISK));
            assert(pinode->i_emapDirExtents == NULL);
            assert(pinode->i_xattrData == NULL);
            assert(pinode->i_ocount == 0);
            if (pinode->i_flags & LC_INODE_REMOVED) {
                prev = &pinode->i_cnext;
                pinode = pinode->i_cnext;
                continue;
            }
            *prev = pinode->i_cnext;
            inode = pinode;
            pinode = pinode->i_cnext;
            inode->i_fs = cfs;
            lc_addInode(cfs, inode, -1, false, NULL, NULL);
            lc_markInodeDirty(inode,
                              S_ISDIR(inode->i_mode) ? LC_INODE_DIRDIRTY :
                              (S_ISREG(inode->i_mode) ?
                               LC_INODE_EMAPDIRTY : 0));
            if (inode->i_ino != fs->fs_root) {
                assert(S_ISDIR(inode->i_mode) || (inode->i_nlink == 1));
                parent = inode->i_parent;
                dirent = lc_getDirent(fs, parent, inode->i_ino, NULL, NULL);
                if (parent == fs->fs_root) {
                    parent = cfs->fs_root;
                }
                dir = lc_getInode(cfs, parent, NULL, false, false);
                lc_dirAdd(dir, dirent->di_ino, dirent->di_mode,
                          dirent->di_name, dirent->di_size);
                if (S_ISDIR(inode->i_mode)) {
                    dir->i_nlink++;
                    size = 0;
                } else if (S_ISREG(inode->i_mode)) {
                    size = sizeof(struct rdata);
                } else if (S_ISLNK(inode->i_mode)) {
                    assert(!(inode->i_flags & LC_INODE_SYMLINK));
                    size = (inode->i_flags & LC_INODE_SHARED) ?
                                            0 : inode->i_size + 1;
                } else {
                    size = 0;
                }
                lc_markInodeDirty(dir, LC_INODE_DIRDIRTY);
                lc_inodeUnlock(dir);
                if (inode->i_rwlock) {
                    lc_memMove(fs, cfs, sizeof(pthread_rwlock_t),
                               LC_MEMTYPE_IRWLOCK);
                }
                lc_memMove(fs, cfs, sizeof(struct inode) + size,
                           LC_MEMTYPE_INODE);
                mcount++;
            }
        }
    }
    if (mcount) {
        fs->fs_icount -= mcount;
        cfs->fs_icount += mcount;
    }
}

/* Move the root inode from one layer to another */
void
lc_moveRootInode(struct gfs *gfs, struct fs *cfs, struct fs *fs) {
    struct inode *dir = cfs->fs_rootInode, *inode;
    ino_t root = dir->i_ino;
    int hash = lc_inodeHash(cfs, root);

    assert(dir->i_ocount == 0);
    assert(dir->i_xattrData == NULL);
    if (cfs->fs_icache[hash].ic_head == dir) {
        cfs->fs_icache[hash].ic_head = dir->i_cnext;
    } else {
        inode = cfs->fs_icache[hash].ic_head;
        while (inode->i_cnext != dir) {
            inode = inode->i_cnext;
        }
        inode->i_cnext = dir->i_cnext;
    }

    /* Insert a dummy inode if this directory is already on disk */
    if (dir->i_flags & LC_INODE_DISK) {
        inode = lc_newInode(cfs, 0, false, false, true, false);
        lc_dinodeInit(inode, root, S_IFDIR | 0755, 0, 0, 0, 0, root);
        lc_addInode(cfs, inode, -1, false, NULL, NULL);
        inode->i_nlink = 0;
        inode->i_flags |= LC_INODE_REMOVED | LC_INODE_DISK;
        cfs->fs_ricount++;
        lc_markInodeDirty(inode, LC_INODE_DIRDIRTY);
    }
    dir->i_fs = fs;
    lc_addInode(fs, dir, -1, false, NULL, NULL);
    lc_markInodeDirty(dir, LC_INODE_DIRDIRTY);
}

/* Swap information between root inodes */
void
lc_swapRootInode(struct fs *fs, struct fs *cfs) {
    struct inode *dir = fs->fs_rootInode, *cdir = cfs->fs_rootInode;
    struct dirent *dirent = dir->i_dirent;
    uint32_t flags = dir->i_flags;
    struct dinode dinode;

    assert(cdir->i_fs == fs);
    assert(dir->i_fs == cfs);
    assert(dir->i_emapDirExtents == NULL);
    assert(!(dir->i_flags & LC_INODE_DISK));
    assert(dir->i_xattrData == NULL);
    assert(cdir->i_xattrData == NULL);
    memcpy(&dinode, &dir->i_dinode, sizeof(struct dinode));
    memcpy(&dir->i_dinode, &cdir->i_dinode, sizeof(struct dinode));
    memcpy(&cdir->i_dinode, &dinode, sizeof(struct dinode));
    dir->i_ino = fs->fs_root;
    dir->i_parent = fs->fs_root;
    cdir->i_ino = cfs->fs_root;
    cdir->i_parent = cfs->fs_root;
    if (cdir->i_emapDirExtents) {
        dir->i_emapDirExtents = cdir->i_emapDirExtents;
        cdir->i_emapDirExtents = NULL;
    }
    dir->i_dirent = cdir->i_dirent;
    cdir->i_dirent = dirent;
    dir->i_flags = cdir->i_flags;
    cdir->i_flags = flags;
    fs->fs_rootInode = cdir;
    cfs->fs_rootInode = dir;
}

/* Switch parent inodes of files in root directory */
void
lc_switchInodeParent(struct fs *fs, ino_t root) {
    struct inode *dir = fs->fs_rootInode;
    bool hashed = (dir->i_flags & LC_INODE_DHASHED);
    int i, max = hashed ? LC_DIRCACHE_SIZE : 1;
    struct dirent *dirent;
    struct inode *inode;

    for (i = 0; i < max; i++) {
        dirent = hashed ? dir->i_hdirent[i] : dir->i_dirent;
        while (dirent) {
            inode = lc_lookupInodeCache(fs, dirent->di_ino, -1);
            if (inode) {
                inode->i_parent = root;
            }
            dirent = dirent->di_next;
        }
    }
}

/* Clone inodes shared with parent layer */
void
lc_cloneInodes(struct gfs *gfs, struct fs *fs, struct fs *pfs) {
    uint64_t i, count = 0, icount = pfs->fs_icount;
    struct inode *inode, *pinode;
    int flags;

    for (i = 0; (i < pfs->fs_icacheSize) && (count < icount); i++) {
        pinode = pfs->fs_icache[i].ic_head;
        while (pinode) {
            count++;
            if ((pinode == pfs->fs_rootInode) ||
                (pinode->i_flags & (LC_INODE_SHARED | LC_INODE_REMOVED))) {
                pinode = pinode->i_cnext;
                continue;
            }
            inode = lc_getInode(fs, pinode->i_ino, NULL, true, true);
            if (inode->i_flags & LC_INODE_SHARED) {
                if (S_ISREG(inode->i_mode)) {
                    lc_copyEmap(gfs, fs, inode);
                    flags = LC_INODE_EMAPDIRTY;
                } else if (S_ISDIR(inode->i_mode)) {
                    lc_dirCopy(inode);
                    flags = LC_INODE_DIRDIRTY;
                } else {
                    flags = 0;
                    assert(S_ISLNK(inode->i_mode));

                    inode->i_target = lc_malloc(fs, inode->i_size + 1,
                                                LC_MEMTYPE_SYMLINK);
                    memcpy(inode->i_target, pinode->i_target,
                           inode->i_size + 1);
                    inode->i_flags |= LC_INODE_SYMLINK;
                    inode->i_flags &= ~LC_INODE_SHARED;
                }
                lc_markInodeDirty(inode, flags);
            }
            lc_inodeUnlock(inode);
            pinode = pinode->i_cnext;
        }
    }
}

