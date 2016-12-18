#include "includes.h"

static bool lc_ftypeStatsEnabled = true;

/* Given an inode number, return the hash index */
static inline int
lc_inodeHash(struct fs *fs, ino_t ino) {
    return ino % fs->fs_icacheSize;
}

/* Allocate and initialize inode hash table */
void
lc_icache_init(struct fs *fs, size_t size) {
    struct icache *icache = lc_malloc(fs, sizeof(struct icache) * size,
                                      LC_MEMTYPE_ICACHE);
    int i;

    for (i = 0; i < size; i++) {
        pthread_mutex_init(&icache[i].ic_lock, NULL);
        icache[i].ic_head = NULL;
    }
    fs->fs_icache = icache;
    fs->fs_icacheSize = size;
}

/* Initialize a disk inode */
static void
lc_dinodeInit(struct inode *inode, ino_t ino, mode_t mode,
              uid_t uid, gid_t gid, dev_t rdev, size_t size, ino_t parent) {
    inode->i_ino = ino;
    inode->i_mode = mode;
    inode->i_nlink = S_ISDIR(mode) ? 2 : 1;
    inode->i_dinode.di_uid = uid;
    inode->i_dinode.di_gid = gid;
    inode->i_dinode.di_rdev = rdev;
    inode->i_size = size;
    inode->i_dinode.di_blocks = 0;
    inode->i_dinode.di_emapdir = LC_INVALID_BLOCK;
    inode->i_dinode.di_extentLength = 0;
    inode->i_dinode.di_xattr = LC_INVALID_BLOCK;
    inode->i_parent = lc_getInodeHandle(parent);
    inode->i_private = S_ISREG(mode);
    lc_updateInodeTimes(inode, true, true);
}

/* Allocate a new inode */
static struct inode *
lc_newInode(struct fs *fs, uint64_t block, uint64_t len, bool reg, bool new) {
    size_t size = sizeof(struct inode) + (reg ? sizeof(struct rdata) : 0);
    struct inode *inode;

    if (len) {
        size += len + 1;
    }
    inode = lc_malloc(fs, size, LC_MEMTYPE_INODE);
    inode->i_block = block;
    inode->i_fs = fs;
    pthread_rwlock_init(&inode->i_rwlock, NULL);
    inode->i_cnext = NULL;
    inode->i_dnext = NULL;
    inode->i_emapDirExtents = NULL;
    inode->i_xattrData = NULL;
    inode->i_ocount = 0;
    inode->i_flags = 0;
    if (reg) {
        inode->i_rdata = (struct rdata *)(((char *)inode) +
                                          sizeof(struct inode));
        memset(inode->i_rdata, 0, sizeof(struct rdata));
    } else {
        inode->i_rdata = NULL;
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
    if (inode->i_fs->fs_frozen) {
        return;
    }
    if (exclusive) {
        pthread_rwlock_wrlock(&inode->i_rwlock);
    } else {
        pthread_rwlock_rdlock(&inode->i_rwlock);
    }
}

/* Unlock the inode */
void
lc_inodeUnlock(struct inode *inode) {
    if (inode->i_fs->fs_frozen) {
        return;
    }
    pthread_rwlock_unlock(&inode->i_rwlock);
}

/* Add an inode to the hash and file system list */
static void
lc_addInode(struct fs *fs, struct inode *inode) {
    int hash = lc_inodeHash(fs, inode->i_ino);

    /* Add the inode to the hash list */
    pthread_mutex_lock(&fs->fs_icache[hash].ic_lock);
    inode->i_cnext = fs->fs_icache[hash].ic_head;
    fs->fs_icache[hash].ic_head = inode;
    pthread_mutex_unlock(&fs->fs_icache[hash].ic_lock);
}

/* Lookup an inode in the hash list */
static struct inode *
lc_lookupInodeCache(struct fs *fs, ino_t ino) {
    int hash = lc_inodeHash(fs, ino);
    struct inode *inode;

    if (fs->fs_icache[hash].ic_head == NULL) {
        return NULL;
    }
    /* XXX Locking not needed right now, as inodes are not removed */
    //pthread_mutex_lock(&fs->fs_icache[hash].ic_lock);
    inode = fs->fs_icache[hash].ic_head;
    while (inode) {
        if (inode->i_ino == ino) {
            break;
        }
        inode = inode->i_cnext;
    }
    //pthread_mutex_unlock(&fs->fs_icache[hash].ic_lock);
    return inode;
}

/* Lookup an inode in the hash list */
static struct inode *
lc_lookupInode(struct fs *fs, ino_t ino) {
    struct gfs *gfs = fs->fs_gfs;

    if (ino == fs->fs_root) {
        return fs->fs_rootInode;
    }
    if (ino == gfs->gfs_snap_root) {
        return gfs->gfs_snap_rootInode;
    }
    return lc_lookupInodeCache(fs, ino);
}

/* Update inode times */
void
lc_updateInodeTimes(struct inode *inode, bool mtime, bool ctime) {
    struct timespec tv;

    clock_gettime(CLOCK_REALTIME, &tv);
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
    struct inode *inode = lc_newInode(fs, LC_INVALID_BLOCK, 0, false, false);

    lc_dinodeInit(inode, root, S_IFDIR | 0755, 0, 0, 0, 0, root);
    lc_addInode(fs, inode);
    fs->fs_rootInode = inode;
    lc_markInodeDirty(inode, true, true, false, false);
}

/* Set up snapshot root inode */
void
lc_setSnapshotRoot(struct gfs *gfs, ino_t ino) {
    struct inode *dir;

    /* Switching layer root is supported just to make tests to run */
    if (gfs->gfs_snap_root) {
        if (gfs->gfs_scount) {
            printf("Warning: Snapshot root changed when snapshots are present\n");
        }
        printf("Switching snapshot root from %ld to %ld\n", gfs->gfs_snap_root, ino);
        gfs->gfs_snap_root = 0;
    }
    dir = lc_getInode(lc_getGlobalFs(gfs), ino, NULL, false, false);
    if (dir) {
        gfs->gfs_snap_root = ino;
        lc_dirConvertHashed(dir->i_fs, dir);
        gfs->gfs_snap_rootInode = dir;
        lc_inodeUnlock(dir);
    }
    printf("snapshot root inode %ld\n", ino);
}

/* Read inodes from an inode block */
static bool
lc_readInodesBlock(struct gfs *gfs, struct fs *fs, uint64_t block,
                   char *buf, void *ibuf) {
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
                            reg, false);
        memcpy(&inode->i_dinode, &buf[offset], sizeof(struct dinode));
        lc_addInode(fs, inode);
        if (reg) {
            lc_emapRead(gfs, fs, inode, ibuf);
        } else if (S_ISDIR(inode->i_mode)) {
            lc_dirRead(gfs, fs, inode, ibuf);
        } else if (len) {
            inode->i_target = (((char *)inode) + sizeof(struct inode));
            memcpy(inode->i_target, &buf[offset + sizeof(struct dinode)], len);
            inode->i_target[len] = 0;
            assert(i == 0);
            i = LC_INODE_BLOCK_MAX;
        }
        lc_xattrRead(gfs, fs, inode, ibuf);
        if (inode->i_ino == fs->fs_root) {
            assert(S_ISDIR(inode->i_mode));
            fs->fs_rootInode = inode;
        }
    }
    return empty;
}

/* Initialize inode table of a file system */
int
lc_readInodes(struct gfs *gfs, struct fs *fs) {
    uint64_t iblock, block = fs->fs_super->sb_inodeBlock;
    bool flush = false, read = true;
    void *ibuf = NULL, *xbuf = NULL;
    struct iblock *buf = NULL;
    int i, j, k, l;

    lc_printf("Reading inodes for fs %d %ld, block %ld\n", fs->fs_gindex, fs->fs_root, block);
    if (block != LC_INVALID_BLOCK) {
        lc_mallocBlockAligned(fs, (void **)&buf, LC_MEMTYPE_BLOCK);
        lc_mallocBlockAligned(fs, (void **)&ibuf, LC_MEMTYPE_BLOCK);
        lc_mallocBlockAligned(fs, (void **)&xbuf, LC_MEMTYPE_BLOCK);
    }
    while (block != LC_INVALID_BLOCK) {
        if (read) {
            //lc_printf("Reading inode table from block %ld\n", block);
            lc_readBlock(gfs, fs, block, buf);
        } else {
            read = true;
        }
        k = LC_IBLOCK_MAX;
        l = LC_IBLOCK_MAX - 1;
        for (i = 0; i < LC_IBLOCK_MAX; i++) {
            iblock = buf->ib_blks[i];
            if (iblock == 0) {
                l = i;
                break;
            }
            if (iblock == LC_INVALID_BLOCK) {
                assert(iblock != LC_INVALID_BLOCK);
                if (k == LC_IBLOCK_MAX) {
                    k = i;
                }
                continue;
            }
            if (lc_readInodesBlock(gfs, fs, iblock, ibuf, xbuf)) {
                lc_freeLayerMetaBlocks(fs, iblock, 1);
                buf->ib_blks[i] = LC_INVALID_BLOCK;
                if (k == LC_IBLOCK_MAX) {
                    k = i;
                }
                flush = true;
            }
        }
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

            /* Free the block if completely empty */
            if ((buf->ib_blks[0] == 0) && (buf->ib_next != LC_INVALID_BLOCK)) {
                iblock = buf->ib_next;
                //lc_printf("Reading inode table from block %ld, moving to %ld\n", iblock, block);
                lc_readBlock(gfs, fs, iblock, buf);
                lc_freeLayerMetaBlocks(fs, iblock, 1);
                read = false;
                continue;
            } else {
                lc_writeBlock(gfs, fs, buf, block);
                flush = false;
            }
        }
        block = buf->ib_next;
        sched_yield();
    }
    assert(fs->fs_rootInode != NULL);
    assert(!flush);
    if (buf) {
        lc_free(fs, buf, LC_BLOCK_SIZE, LC_MEMTYPE_BLOCK);
        lc_free(fs, ibuf, LC_BLOCK_SIZE, LC_MEMTYPE_BLOCK);
        lc_free(fs, xbuf, LC_BLOCK_SIZE, LC_MEMTYPE_BLOCK);
    }
    return 0;
}

/* Free an inode and associated resources */
static void
lc_freeInode(struct inode *inode) {
    size_t size = sizeof(struct inode);
    struct fs *fs = inode->i_fs;

    if (S_ISREG(inode->i_mode)) {
        lc_truncPages(inode, 0, false);
        assert(inode->i_page == NULL);
        assert(inode->i_emap == NULL);
        assert(inode->i_pcount == 0);
        assert(inode->i_dpcount == 0);
        size += sizeof(struct rdata);
    } else if (S_ISDIR(inode->i_mode)) {
        lc_dirFree(inode);
    } else if (S_ISLNK(inode->i_mode)) {
        inode->i_target = NULL;
        if (!(inode->i_flags & LC_INODE_SHARED)) {
            size += inode->i_size + 1;
        }
    }
    lc_xattrFree(inode);
    assert(inode->i_xattrData == NULL);
    pthread_rwlock_destroy(&inode->i_rwlock);
    lc_blockFreeExtents(fs, inode->i_emapDirExtents, false, false, true);
    lc_free(fs, inode, size, LC_MEMTYPE_INODE);
}

/* Invalidate dirty inode pages */
void
lc_invalidateInodePages(struct gfs *gfs, struct fs *fs) {
    struct page *page;

    if (fs->fs_inodePagesCount) {
        page = fs->fs_inodePages;
        fs->fs_inodePages = NULL;
        fs->fs_inodePagesCount = 0;
        lc_releasePages(gfs, fs, page);
    }
}

/* Flush dirty inodes */
static void
lc_flushInodePages(struct gfs *gfs, struct fs *fs) {
    //lc_printf("lc_flushInodePages: flushing %ld inode pages\n", fs->fs_inodePagesCount);
    lc_flushPageCluster(gfs, fs, fs->fs_inodePages, fs->fs_inodePagesCount);
    fs->fs_inodePages = NULL;
    fs->fs_inodePagesCount = 0;
}

/* Allocate a block for an inode */
static bool
fs_allocInodeBlock(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    uint64_t block;
    bool allocated = false;

    pthread_mutex_lock(&fs->fs_alock);
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
        inode->i_block = fs->fs_blockInodes++;
        fs->fs_blockInodesCount--;
        fs->fs_inodeBlocks->ib_blks[fs->fs_inodeIndex++] = inode->i_block;
        fs->fs_inodeBlockIndex = 1;
        allocated = true;
    } else {
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

/* Flush a dirty inode to disk */
int
lc_flushInode(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    bool written = false, allocated = true;
    struct page *page = NULL;
    uint64_t block;
    off_t offset;

    assert(inode->i_fs == fs);
    if (inode->i_flags & LC_INODE_XATTRDIRTY) {
        lc_xattrFlush(gfs, fs, inode);
    }

    if (inode->i_flags & LC_INODE_EMAPDIRTY) {
        lc_emapFlush(gfs, fs, inode);
    } else if (inode->i_flags & LC_INODE_DIRDIRTY) {
        lc_dirFlush(gfs, fs, inode);
    }

    /* Write out a dirty inode */
    if (inode->i_flags & LC_INODE_DIRTY) {
        if (inode->i_flags & LC_INODE_REMOVED) {
            assert(inode->i_extentLength == 0);

            /* Free metadata blocks allocated to the inode */
            lc_blockFreeExtents(fs, inode->i_emapDirExtents,
                                true, false, true);
            inode->i_emapDirExtents = NULL;
            inode->i_emapDirBlock = LC_INVALID_BLOCK;
            if (inode->i_xattrData && inode->i_xattrExtents) {
                lc_blockFreeExtents(fs, inode->i_xattrExtents,
                                    true, false, true);
                inode->i_xattrExtents = NULL;
            }
            inode->i_xattrBlock = LC_INVALID_BLOCK;
        }

        /* An removed inode with a disk copy, needs to be written out so that
         * it would be considered removed when the layer is remounted.
         */
        if (!(inode->i_flags & LC_INODE_REMOVED) ||
            (inode->i_block != LC_INVALID_BLOCK)) {
            allocated = false;
            if (inode->i_block == LC_INVALID_BLOCK) {
                allocated = fs_allocInodeBlock(gfs, fs, inode);
            }
            offset = (inode->i_block >> LC_DINODE_INDEX) * LC_DINODE_SIZE;
            block = inode->i_block & LC_DINODE_BLOCK;
            //lc_printf("inode %ld block %ld offset %ld block %ld\n", inode->i_ino, inode->i_block, offset, block);
            assert(offset < LC_BLOCK_SIZE);
            written = true;
            assert(!(inode->i_flags & LC_INODE_REMOVED) ||
                   (inode->i_nlink == 0));

            //lc_printf("Writing inode %ld to block %ld\n", inode->i_ino, inode->i_block);
            if (allocated) {
                assert(offset == 0);
                page = lc_getPageNewData(fs, block);
                memset(&page->p_data[sizeof(struct dinode)], 0,
                       LC_BLOCK_SIZE - sizeof(struct dinode));
            } else {
                page = lc_getPage(fs, block, true);
            }
            memcpy(&page->p_data[offset], &inode->i_dinode,
                   sizeof(struct dinode));
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
                    lc_flushInodePages(gfs, fs);
                }
                page->p_dnext = fs->fs_inodePages;
                fs->fs_inodePages = page;
                fs->fs_inodePagesCount++;
                if (fs->fs_inodePagesCount >= LC_CLUSTER_SIZE) {
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

/* Sync all dirty inodes */
void
lc_syncInodes(struct gfs *gfs, struct fs *fs) {
    struct inode *inode;
    uint64_t count = 0;
    int i;

    lc_printf("Syncing inodes for fs %d %ld\n", fs->fs_gindex, fs->fs_root);
    inode = fs->fs_rootInode;
    if (inode && !fs->fs_removed && lc_inodeDirty(inode)) {
        count += lc_flushInode(gfs, fs, inode);
    }
    if (fs == lc_getGlobalFs(gfs)) {
        inode = gfs->gfs_snap_rootInode;
        if (inode && !fs->fs_removed && lc_inodeDirty(inode)) {
            count += lc_flushInode(gfs, fs, inode);
        }
    }
    for (i = 0; i < fs->fs_icacheSize; i++) {
        inode = fs->fs_icache[i].ic_head;
        while (inode && !fs->fs_removed) {
            if ((inode->i_flags & LC_INODE_REMOVED) &&
                S_ISREG(inode->i_mode) &&
                inode->i_size) {
                assert(lc_inodeDirty(inode));
                lc_truncPages(inode, 0, true);
            }
            if (lc_inodeDirty(inode)) {
                count += lc_flushInode(gfs, fs, inode);
            }
            inode = inode->i_cnext;
        }
        sched_yield();
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

/* Destroy inodes belong to a file system */
void
lc_destroyInodes(struct fs *fs, bool remove) {
    uint64_t icount = 0, rcount = 0;
    struct inode *inode;
    int i;

    /* Take the inode off the hash list */
    for (i = 0; i < fs->fs_icacheSize; i++) {
        /* XXX Lock is not needed as the file system is locked for exclusive
         * access
         * */
        //pthread_mutex_lock(&fs->fs_icache[i].ic_lock);
        while ((inode = fs->fs_icache[i].ic_head)) {
            fs->fs_icache[i].ic_head = inode->i_cnext;
            if (!(inode->i_flags & LC_INODE_REMOVED)) {
                rcount++;
            }
            lc_freeInode(inode);
            icount++;
        }
        assert(fs->fs_icache[i].ic_head == NULL);
        //pthread_mutex_unlock(&fs->fs_icache[i].ic_lock);
        pthread_mutex_destroy(&fs->fs_icache[i].ic_lock);
        sched_yield();
    }

    /* XXX reuse this cache for another file system */
    lc_free(fs, fs->fs_icache, sizeof(struct icache) * fs->fs_icacheSize,
            LC_MEMTYPE_ICACHE);
    if (remove && icount) {
        __sync_sub_and_fetch(&fs->fs_gfs->gfs_super->sb_inodes, rcount);
    }
    if (icount) {
        __sync_sub_and_fetch(&fs->fs_icount, icount);
    }
}

/* Clone an inode from a parent layer */
struct inode *
lc_cloneInode(struct fs *fs, struct inode *parent, ino_t ino) {
    bool reg = S_ISREG(parent->i_mode);
    bool emap = false, dir = false, xattr;
    struct inode *inode;

    inode = lc_newInode(fs, LC_INVALID_BLOCK, 0, reg, false);
    memcpy(&inode->i_dinode, &parent->i_dinode, sizeof(struct dinode));

    if (reg) {
        assert(parent->i_page == NULL);
        assert(parent->i_dpcount == 0);

        /* Share pages initially */
        if (parent->i_dinode.di_blocks) {
            if (parent->i_extentLength) {
                inode->i_extentBlock = parent->i_extentBlock;
                inode->i_extentLength = parent->i_extentLength;
            } else {
                inode->i_emap = parent->i_emap;
                inode->i_flags |= LC_INODE_SHARED;
                emap = true;
            }
        } else {
            inode->i_private = true;
        }
    } else if (S_ISDIR(inode->i_mode)) {
        if (parent->i_dirent) {
            inode->i_dirent = parent->i_dirent;
            inode->i_flags |= LC_INODE_SHARED;
            if (parent->i_flags & LC_INODE_DHASHED) {
                inode->i_flags |= LC_INODE_DHASHED;
            }
            dir = true;
        }
    } else if (S_ISLNK(inode->i_mode)) {
        inode->i_target = parent->i_target;
        inode->i_flags |= LC_INODE_SHARED;
    }
    inode->i_parent = (parent->i_parent == parent->i_fs->fs_root) ?
                      fs->fs_root : parent->i_parent;
    xattr = lc_xattrCopy(inode, parent);
    lc_addInode(fs, inode);
    lc_markInodeDirty(inode, true, dir, emap, xattr);
    __sync_add_and_fetch(&fs->fs_gfs->gfs_clones, 1);
    lc_updateFtypeStats(fs, inode->i_mode, true);
    return inode;
}

/* Lookup the requested inode in the chain */
static struct inode *
lc_getInodeParent(struct fs *fs, ino_t inum, bool copy) {
    struct inode *inode, *parent;
    struct fs *pfs;

    /* XXX Reduce the time this lock is held */
    pthread_mutex_lock(fs->fs_ilock);
    inode = lc_lookupInodeCache(fs, inum);
    if (inode == NULL) {
        pfs = fs->fs_parent;
        while (pfs) {
            parent = lc_lookupInodeCache(pfs, inum);
            if (parent != NULL) {

                /* Do not clone if the inode is removed in a parent layer */
                if (!(parent->i_flags & LC_INODE_REMOVED)) {

                    /* Clone the inode only when modified */
                    if (copy) {
                        assert(fs->fs_snap == NULL);
                        inode = lc_cloneInode(fs, parent, inum);
                    } else {
                        /* XXX Remember this for future lookup */
                        inode = parent;
                    }
                }
                break;
            }
            pfs = pfs->fs_parent;
        }
    }
    pthread_mutex_unlock(fs->fs_ilock);
    return inode;
}

/* Get an inode locked in the requested mode */
struct inode *
lc_getInode(struct fs *fs, ino_t ino, struct inode *handle,
             bool copy, bool exclusive) {
    ino_t inum = lc_getInodeHandle(ino);
    struct inode *inode;

    assert(!fs->fs_removed);

    /* Check if the file handle points to the inode */
    if (handle) {
        inode = handle;
        if (!copy || (inode->i_fs == fs)) {
            assert(inode->i_ino == inum);
            lc_inodeLock(inode, exclusive);
            return inode;
        }
    }

    /* Check if the file system has the inode or not */
    inode = lc_lookupInode(fs, inum);
    if (inode) {
        lc_inodeLock(inode, exclusive);
        return inode;
    }

    /* Lookup inode in the parent chain */
    if (fs->fs_parent) {
        inode = lc_getInodeParent(fs, inum, copy);
    }

    /* Now lock the inode */
    if (inode) {
        lc_inodeLock(inode, exclusive);
    } else {
        lc_printf("Inode is NULL, fs gindex %d root %ld ino %ld\n",
                   fs->fs_gindex, fs->fs_root, ino);
    }
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
    int len = (target != NULL) ? len = strlen(target) : 0;
    struct inode *inode;

    inode = lc_newInode(fs, LC_INVALID_BLOCK, len, S_ISREG(mode), true);
    if (len) {
        inode->i_target = (((char *)inode) + sizeof(struct inode));
        memcpy(inode->i_target, target, len);
        inode->i_target[len] = 0;
    }
    lc_dinodeInit(inode, lc_inodeAlloc(fs), mode, uid, gid, rdev, len, parent);
    lc_updateFtypeStats(fs, mode, true);
    lc_addInode(fs, inode);
    lc_inodeLock(inode, true);
    return inode;
}
