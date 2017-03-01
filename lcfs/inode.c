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
lc_newInode(struct fs *fs, uint64_t block, uint64_t len, bool reg, bool new,
            bool lock) {
    size_t size = sizeof(struct inode) + (reg ? sizeof(struct rdata) : 0);
    struct inode *inode;
    struct rdata *rdata;

    if (len) {
        size += len + 1;
    }
    inode = lc_malloc(fs, size, LC_MEMTYPE_INODE);
    inode->i_block = block;
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
    inode->i_flags = 0;
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
                return inode;
            }
        }
        inode = new;
    }
    assert(lc_lookupInodeCache(fs, ino, hash) == NULL);

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
    struct inode *dir = lc_newInode(fs, LC_INVALID_BLOCK, 0, false, false,
                                    true);

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

/* Read inodes from an inode block */
static bool
lc_readInodesBlock(struct gfs *gfs, struct fs *fs, uint64_t block,
                   char *buf, void *ibuf, bool lock) {
    bool empty = true, reg;
    struct inode *inode;
    uint64_t i, len;
    off_t offset;

    //lc_printf("Reading inode from block %ld\n", block);
    lc_readBlock(gfs, fs, block, buf);
    for (i = 0; i < LC_INODE_BLOCK_MAX; i++) {
        offset = i * LC_DINODE_SIZE;
        inode = (struct inode *)&buf[offset];

        /* Skip removed/unused inodes */
        if (inode->i_nlink == 0) {
            continue;
        }
        empty = false;
        reg = S_ISREG(inode->i_mode);
        len = S_ISLNK(inode->i_mode) ? inode->i_size : 0;
        inode = lc_newInode(fs, (i << LC_DINODE_INDEX) | block, len,
                            reg, false, lock);
        memcpy(&inode->i_dinode, &buf[offset], sizeof(struct dinode));
        lc_addInode(fs, inode, -1, false, NULL, NULL);
        if (reg) {

            /* Read emap of fragmented regular files */
            lc_emapRead(gfs, fs, inode, ibuf);
        } else if (S_ISDIR(inode->i_mode)) {

            /* Read directory entries */
            lc_dirRead(gfs, fs, inode, ibuf);
        } else if (len) {

            /* Setup target of a symbolic link */
            inode->i_target = (((char *)inode) + sizeof(struct inode));
            memcpy(inode->i_target, &buf[offset + sizeof(struct dinode)], len);
            inode->i_target[len] = 0;
            assert(i == 0);
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
    bool flush = false, read = true, lock = !fs->fs_frozen;
    uint64_t iblock, block = fs->fs_super->sb_inodeBlock;
    void *ibuf = NULL, *xbuf = NULL;
    struct iblock *buf = NULL;
    int i, j, k, l;

    lc_printf("Reading inodes for fs %d %ld, block %ld\n", fs->fs_gindex, fs->fs_root, block);
    assert(block != LC_INVALID_BLOCK);
    lc_mallocBlockAligned(fs, (void **)&buf, LC_MEMTYPE_BLOCK);
    lc_mallocBlockAligned(fs, (void **)&ibuf, LC_MEMTYPE_BLOCK);
    lc_mallocBlockAligned(fs, (void **)&xbuf, LC_MEMTYPE_BLOCK);

    /* Read inode blocks linked from the super block */
    while (block != LC_INVALID_BLOCK) {
        if (read) {
            //lc_printf("Reading inode table from block %ld\n", block);
            lc_readBlock(gfs, fs, block, buf);
            lc_verifyBlock(buf, &buf->ib_crc);
            assert(buf->ib_magic == LC_INODE_MAGIC);
        } else {
            read = true;
        }
        k = LC_IBLOCK_MAX;
        l = LC_IBLOCK_MAX - 1;

        /* Process inode blocks from the block read */
        for (i = 0; i < LC_IBLOCK_MAX; i++) {
            iblock = buf->ib_blks[i];
            if (iblock == 0) {
                l = i;
                break;
            }

            assert(iblock != LC_INVALID_BLOCK);
            if (lc_readInodesBlock(gfs, fs, iblock, ibuf, xbuf, lock)) {
                lc_freeLayerMetaBlocks(fs, iblock, 1);

                /* If the inode block is completely empty, insert a dummy entry
                 * and remember first such entry in the block.
                 */
                buf->ib_blks[i] = LC_INVALID_BLOCK;
                if (k == LC_IBLOCK_MAX) {
                    k = i;
                }
                flush = true;
            }
        }

        /* Flush if some inode blocks are removed */
        if (flush) {

            /* Compact the block by removing all LC_INVALID_BLOCK entries */
            for (i = k; i < LC_IBLOCK_MAX; i++) {
                iblock = buf->ib_blks[i];
                if (iblock == 0) {
                    break;
                }
                if (iblock == LC_INVALID_BLOCK) {
                    for (j = l; j >= i; j--) {
                        iblock = buf->ib_blks[j];
                        if (iblock) {
                            l = j - 1;
                            buf->ib_blks[j] = 0;
                            if (iblock != LC_INVALID_BLOCK) {
                                buf->ib_blks[i] = iblock;
                                break;
                            }
                        }
                    }
                }
            }

            /* Free next inode block if current block completely empty after
             * copying in the next block to current block.
             */
            if ((buf->ib_blks[0] == 0) && (buf->ib_next != LC_INVALID_BLOCK)) {
                iblock = buf->ib_next;
                //lc_printf("Reading inode table from block %ld, moving to %ld\n", iblock, block);
                lc_readBlock(gfs, fs, iblock, buf);
                lc_verifyBlock(buf, &buf->ib_crc);
                assert(buf->ib_magic == LC_INODE_MAGIC);
                lc_freeLayerMetaBlocks(fs, iblock, 1);
                read = false;
                continue;
            }
            lc_updateCRC(buf, &buf->ib_crc);
            lc_writeBlock(gfs, fs, buf, block);
            flush = false;
        }
        block = buf->ib_next;
    }
    assert(fs->fs_rootInode != NULL);
    assert(!flush);
    lc_free(fs, buf, LC_BLOCK_SIZE, LC_MEMTYPE_BLOCK);
    lc_free(fs, ibuf, LC_BLOCK_SIZE, LC_MEMTYPE_BLOCK);
    lc_free(fs, xbuf, LC_BLOCK_SIZE, LC_MEMTYPE_BLOCK);
}

/* Invalidate dirty inode pages */
void
lc_invalidateInodePages(struct gfs *gfs, struct fs *fs) {
    struct page *page;

    if (fs->fs_inodePagesCount) {
        page = fs->fs_inodePages;
        fs->fs_inodePages = NULL;
        fs->fs_inodePagesCount = 0;
        lc_releasePages(gfs, fs, page, true);
    }
}

/* Flush dirty inodes */
static void
lc_flushInodePages(struct gfs *gfs, struct fs *fs) {
    lc_flushPageCluster(gfs, fs, fs->fs_inodePages,
                        fs->fs_inodePagesCount, false);
    fs->fs_inodePages = NULL;
    fs->fs_inodePagesCount = 0;
}

/* Allocate a slot in an inode block for storing an inode */
static bool
fs_allocInodeBlock(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    uint64_t block;
    bool allocated = false;

    pthread_mutex_lock(&fs->fs_alock);

    /* Allocate new inode blocks if needed.  Allocate a few together so that
     * inode blocks can be somewhat contiguous on disk.
     */
    if ((fs->fs_inodeBlocks == NULL) ||
        (fs->fs_inodeIndex >= LC_IBLOCK_MAX)) {
        lc_newInodeBlock(gfs, fs);
    }

    /* Start with a new block for symbolic links */
    if (S_ISLNK(inode->i_mode)) {
        fs->fs_inodeBlockIndex = 0;
    }
    if ((fs->fs_inodeBlockIndex == 0) ||
        (fs->fs_inodeBlockIndex >= LC_INODE_BLOCK_MAX)) {
        if (fs->fs_blockInodesCount == 0) {

            /* Reserve a few blocks for inodes */
            pthread_mutex_unlock(&fs->fs_alock);
            block = lc_blockAllocExact(fs, LC_INODE_CLUSTER_SIZE, true, true);
            pthread_mutex_lock(&fs->fs_alock);
            assert(fs->fs_blockInodesCount == 0);
            fs->fs_blockInodesCount = LC_INODE_CLUSTER_SIZE;
            fs->fs_blockInodes = block;
        }
        assert(fs->fs_blockInodes != LC_INVALID_BLOCK);
        assert(fs->fs_blockInodes != 0);
        assert(fs->fs_blockInodesCount > 0);

        /* Pick up the next available inode block */
        inode->i_block = fs->fs_blockInodes++;
        fs->fs_blockInodesCount--;
        fs->fs_inodeBlocks->ib_blks[fs->fs_inodeIndex++] = inode->i_block;
        fs->fs_inodeBlockIndex = 1;
        allocated = true;
    } else {

        /* Pick next available slot in the current inode block */
        assert(fs->fs_blockInodes != LC_INVALID_BLOCK);
        assert(fs->fs_blockInodes != 0);
        inode->i_block = ((uint64_t)fs->fs_inodeBlockIndex <<
                          LC_DINODE_INDEX) | (fs->fs_blockInodes - 1);
        fs->fs_inodeBlockIndex++;
    }

    /* Use the whole block for symbolic links */
    if (S_ISLNK(inode->i_mode)) {
        fs->fs_inodeBlockIndex = LC_INODE_BLOCK_MAX;
    }
    pthread_mutex_unlock(&fs->fs_alock);
    return allocated;
}

/* Free metadata extents allocated to an inode */
static void
lc_inodeFreeMetaExtents(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    assert(inode->i_extentLength == 0);

    /* Free metadata blocks allocated to the inode */
    if (inode->i_emapDirExtents) {
        lc_blockFreeExtents(gfs, fs, inode->i_emapDirExtents,
                            LC_EXTENT_EFREE | LC_EXTENT_LAYER);
        inode->i_emapDirExtents = NULL;
    }
    inode->i_emapDirBlock = LC_INVALID_BLOCK;
    if (inode->i_xattrData && inode->i_xattrExtents) {
        lc_blockFreeExtents(gfs, fs, inode->i_xattrExtents,
                            LC_EXTENT_EFREE | LC_EXTENT_LAYER);
        inode->i_xattrExtents = NULL;
    }
    inode->i_xattrBlock = LC_INVALID_BLOCK;
}

/* Flush a dirty inode to disk */
int
lc_flushInode(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    bool written = false, allocated = true;
    struct page *page = NULL;
    uint64_t block;
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
        if (inode->i_flags & LC_INODE_REMOVED) {
            lc_inodeFreeMetaExtents(gfs, fs, inode);
        }

        /* A removed inode with a disk copy, needs to be written out so that
         * it would be considered removed when the layer is remounted.
         */
        if (!(inode->i_flags & LC_INODE_REMOVED) ||
            (inode->i_block != LC_INVALID_BLOCK)) {
            allocated = false;
            if (inode->i_block == LC_INVALID_BLOCK) {

                /* Find an inode block with a slot for this inode */
                allocated = fs_allocInodeBlock(gfs, fs, inode);
            }
            offset = (inode->i_block >> LC_DINODE_INDEX) * LC_DINODE_SIZE;
            block = inode->i_block & LC_DINODE_BLOCK;
            assert(offset < LC_BLOCK_SIZE);
            written = true;
            assert(!(inode->i_flags & LC_INODE_REMOVED) ||
                   (inode->i_nlink == 0));

            //lc_printf("Writing inode %ld to block %ld\n", inode->i_ino, inode->i_block);
            if (allocated) {
                assert(offset == 0);
                page = lc_getPageNewData(fs, block, NULL);

                /* Zero out rest of the block */
                memset(&page->p_data[sizeof(struct dinode)], 0,
                       LC_BLOCK_SIZE - sizeof(struct dinode));
            } else {
                page = lc_getPage(fs, block, NULL, true);
            }
            memcpy(&page->p_data[offset], &inode->i_dinode,
                   sizeof(struct dinode));

            /* Store the target of the symbolic link in the same block */
            if (S_ISLNK(inode->i_mode)) {
                assert(inode->i_block == block);
                memcpy(&page->p_data[sizeof(struct dinode)], inode->i_target,
                       inode->i_size);
            }

            /* Add the page to list of inode pages pending write, if the page
             * is not part of that already.
             */
            if ((page->p_dnext == NULL) && (fs->fs_inodePages != page)) {
                page->p_dvalid = 1;
                if (fs->fs_inodePages &&
                    (page->p_block != (fs->fs_inodePages->p_block + 1))) {

                    /* Flush inode pages if the new block is not immediately
                     * after the previous dirty block.
                     */
                    lc_flushInodePages(gfs, fs);
                }
                page->p_dnext = fs->fs_inodePages;
                fs->fs_inodePages = page;
                fs->fs_inodePagesCount++;
                if (fs->fs_inodePagesCount >= LC_WRITE_CLUSTER_SIZE) {
                    lc_flushInodePages(gfs, fs);
                }
            } else {
                assert(page->p_dvalid);
                lc_releasePage(gfs, fs, page, false);
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
                !(inode->i_flags & LC_INODE_NOTRUNC)) {
                assert(inode->i_ocount == 0);
                assert(inode->i_block == LC_INVALID_BLOCK);
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
                lc_flushPages(gfs, fs, inode, false, true, false);
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
        assert(fs->fs_ricount == rcount);
        fs->fs_icount -= rcount;
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
lc_syncInodes(struct gfs *gfs, struct fs *fs) {
    struct inode *inode;
    uint64_t count = 0;
    int i;

    lc_printf("Syncing inodes for fs %d %ld\n", fs->fs_gindex, fs->fs_root);

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
    for (i = 0; (i < fs->fs_icacheSize) && !fs->fs_removed; i++) {
        inode = fs->fs_icache[i].ic_head;
        while (inode && !fs->fs_removed) {
            if ((inode->i_flags & LC_INODE_REMOVED) &&
                S_ISREG(inode->i_mode) &&
                inode->i_size) {
                assert(lc_inodeDirty(inode));

                /* Truncate pages of a removed inode on umount */
                lc_truncateFile(inode, 0, true);
                inode->i_size = 0;
            }
            if (lc_inodeDirty(inode)) {
                count += lc_flushInode(gfs, fs, inode);
            }
            inode = inode->i_cnext;
        }
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
    if (icount) {
        __sync_sub_and_fetch(&fs->fs_icount, icount);
    }
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
    new = lc_newInode(fs, LC_INVALID_BLOCK, 0, reg, false, true);
    memcpy(&new->i_dinode, &parent->i_dinode, sizeof(struct dinode));
    lc_inodeLock(new, true);
    inode = lc_addInode(fs, new, hash, true, new, last);
    if (inode != new) {
        lc_inodeLock(inode, exclusive);
        return inode;
    }
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
            inode->i_private = true;
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

    inode = lc_newInode(fs, LC_INVALID_BLOCK, len, S_ISREG(mode), true, true);
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
            if (pinode->i_flags & LC_INODE_REMOVED) {
                prev = &pinode->i_cnext;
                pinode = pinode->i_cnext;
                continue;
            }
            assert(pinode->i_dinode.di_blocks == 0);
            assert(pinode->i_block == LC_INVALID_BLOCK);
            assert(pinode->i_emapDirExtents == NULL);
            assert(pinode->i_xattrData == NULL);
            assert(pinode->i_ocount == 0);
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
        __sync_sub_and_fetch(&fs->fs_icount, mcount);
        __sync_add_and_fetch(&cfs->fs_icount, mcount);
    }
}

/* Move the root inode from one layer to another */
void
lc_moveRootInode(struct fs *cfs, struct fs *fs) {
    struct inode *dir = cfs->fs_rootInode, *inode;
    int hash = lc_inodeHash(cfs, dir->i_ino);

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
    dir->i_fs = fs;
    lc_addInode(fs, dir, -1, false, NULL, NULL);
    lc_markInodeDirty(dir, LC_INODE_DIRDIRTY);
}

/* Swap information between root inodes */
void
lc_swapRootInode(struct fs *fs, struct fs *cfs) {
    struct inode *dir = fs->fs_rootInode, *cdir = cfs->fs_rootInode;
    struct extent *extent = dir->i_emapDirExtents;
    struct dirent *dirent = dir->i_dirent;
    uint32_t flags = dir->i_flags;
    uint64_t block = dir->i_block;
    struct dinode dinode;

    assert(cdir->i_fs == fs);
    assert(dir->i_fs == cfs);
    memcpy(&dinode, &dir->i_dinode, sizeof(struct dinode));
    memcpy(&dir->i_dinode, &cdir->i_dinode, sizeof(struct dinode));
    memcpy(&cdir->i_dinode, &dinode, sizeof(struct dinode));
    dir->i_ino = fs->fs_root;
    dir->i_parent = fs->fs_root;
    cdir->i_ino = cfs->fs_root;
    cdir->i_parent = cfs->fs_root;
    assert((block == LC_INVALID_BLOCK) || ((block >> LC_DINODE_INDEX) == 0));
    assert((cdir->i_block == LC_INVALID_BLOCK) ||
           ((cdir->i_block >> LC_DINODE_INDEX) == 0));
    dir->i_block = cdir->i_block;
    cdir->i_block = block;
    dir->i_emapDirExtents = cdir->i_emapDirExtents;
    cdir->i_emapDirExtents = extent;
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

