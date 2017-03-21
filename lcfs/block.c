#include "includes.h"

/* Space reserved as a percentage.  When available free space reaches this
 * threshold new writes and new layers are failed.
 */
#define LC_RESERVED_BLOCKS  10ul

/* Number of blocks reserved by a layer from the global pool at a time */
#define LC_BLOCK_RESERVE    8192

/* Minimum number of blocks attempted to reclaim in one pass */
#define LC_RECLAIM_BLOCKS   10

/* Initializes the block allocator */
void
lc_blockAllocatorInit(struct gfs *gfs, struct fs *fs) {
    struct extent *extent;

    /* Initialize a space extent covering the whole device */
    extent = lc_malloc(fs, sizeof(struct extent), LC_MEMTYPE_EXTENT);
    lc_initExtent(gfs, extent, LC_EXTENT_SPACE, LC_START_BLOCK,
                  0, gfs->gfs_super->sb_tblocks - LC_START_BLOCK, NULL);
    gfs->gfs_extents = extent;
    gfs->gfs_blocksReserved = (gfs->gfs_super->sb_tblocks *
                               LC_RESERVED_BLOCKS) / 100ul;
}

/* Release any unused reserved blocks */
static uint64_t
lc_releaseReservedBlocks(struct gfs *gfs, struct fs *fs) {
    uint64_t freed;

    if (fs->fs_extents) {
        freed = lc_blockFreeExtents(gfs, fs, fs->fs_extents,
                                    LC_EXTENT_EFREE | LC_EXTENT_REUSE);
        assert(fs->fs_reservedBlocks == freed);
        fs->fs_reservedBlocks -= freed;
        fs->fs_extents = NULL;
    } else {
        freed = 0;
    }
    return freed;
}

/* Reclaim reserved space from all layers */
static uint64_t
lc_reclaimSpace(struct gfs *gfs) {
    bool queued = false;
    uint64_t count = 0;
    struct fs *fs;
    int i;

    rcu_register_thread();
    rcu_read_lock();
    for (i = 0; i <= gfs->gfs_scount; i++) {
        fs = rcu_dereference(gfs->gfs_fs[i]);
        if (fs) {

            /* Queue a checkpoint */
            if (!queued && fs->fs_fextents) {
                lc_layerChanged(gfs, false, true);
                queued = true;
            }

            /* Release any reserved blocks */
            if (fs->fs_extents && !lc_tryLock(fs, false)) {
                rcu_read_unlock();
                if (fs->fs_extents) {
                    pthread_mutex_lock(&fs->fs_alock);
                    count += lc_releaseReservedBlocks(gfs, fs);
                    pthread_mutex_unlock(&fs->fs_alock);
                }
                lc_unlock(fs);
                rcu_read_lock();
                if (count >= LC_RECLAIM_BLOCKS) {
                    break;
                }
            }
        }
    }
    rcu_read_unlock();
    rcu_unregister_thread();
    return count;
}

/* Check if file system has enough space for the operation to proceed */
bool
lc_hasSpace(struct gfs *gfs, bool layer) {
    while (gfs->gfs_super->sb_tblocks <=
           (gfs->gfs_super->sb_blocks + gfs->gfs_blocksReserved +
            gfs->gfs_dcount)) {

        /* Try to reclaim reserved space from all layers */
        if (lc_reclaimSpace(gfs) == 0) {
            break;
        }
    }
    return gfs->gfs_super->sb_tblocks >
           (gfs->gfs_super->sb_blocks + gfs->gfs_blocksReserved +
            gfs->gfs_dcount + (layer ? LC_LAYER_MIN_BLOCKS : 0));
}

/* Add an extent to an extent list tracking space */
void
lc_addSpaceExtent(struct gfs *gfs, struct fs *fs, struct extent **extents,
                  uint64_t start, uint64_t count, bool sort) {
    assert(start && count);
    assert(start != LC_INVALID_BLOCK);
    lc_addExtent(gfs, fs, extents, start, 0, count, sort);
}

/* Allocate from a free list of extents */
static uint64_t
lc_allocateBlock(struct gfs *gfs, struct fs *fs, uint64_t count, bool layer) {
    struct extent *extent, **prev;
    uint64_t block;
    bool release;

    prev = layer ? &fs->fs_extents : &gfs->gfs_extents;
    extent = *prev;
    while (extent) {
        if (lc_getExtentCount(extent) >= count) {

            /* Remove the requested number of blocks from the extent */
            block = lc_getExtentStart(extent);
            lc_incrExtentStart(NULL, extent, count);
            release = lc_decrExtentCount(gfs, extent, count);

            /* Free the extent if it is fully consumed */
            if (release) {
                lc_freeExtent(gfs, fs, extent, prev, layer);
            }

            if (layer) {

                /* Update reserved pool and register this extent in the
                 * allocated list of extents.
                 */
                assert(fs->fs_reservedBlocks >= count);
                fs->fs_reservedBlocks -= count;
                if (fs != lc_getGlobalFs(gfs)) {
                    lc_addSpaceExtent(gfs, fs, &fs->fs_aextents, block,
                                      count, true);
                    fs->fs_blocks += count;
                }
            } else {

                /* Update global usage */
                gfs->gfs_super->sb_blocks += count;
                assert(gfs->gfs_super->sb_tblocks > gfs->gfs_super->sb_blocks);
            }
            assert(block < gfs->gfs_super->sb_tblocks);
            return block;
        }
        prev = &extent->ex_next;
        extent = extent->ex_next;
    }
    return LC_INVALID_BLOCK;
}

/* Find a run of free blocks from the free extent list */
static uint64_t
lc_findFreeBlock(struct gfs *gfs, struct fs *fs,
                 uint64_t count, bool reserve, bool layer) {
    uint64_t block, rsize;
    struct fs *rfs;

    /* Check if an extent with enough free blocks available */
    block = lc_allocateBlock(gfs, fs, count, layer);

    /* If the layer does not have any reserved chunks, get one */
    if ((block == LC_INVALID_BLOCK) && layer) {
        rsize = (!reserve || (count > LC_BLOCK_RESERVE)) ?
                count : LC_BLOCK_RESERVE;
        pthread_mutex_lock(&gfs->gfs_alock);
        block = lc_findFreeBlock(gfs, fs, rsize, false, false);

        /* If bigger reservation attempt failed, try with actual request size
         */
        if ((block == LC_INVALID_BLOCK) && (count < rsize)) {
            rsize = count;
            block = lc_findFreeBlock(gfs, fs, rsize, false, false);
        }
        pthread_mutex_unlock(&gfs->gfs_alock);
        if (block != LC_INVALID_BLOCK) {
            rfs = lc_getGlobalFs(gfs);
            if (fs != rfs) {

                /* Track the allocated space for the layer */
                lc_addSpaceExtent(gfs, fs, &fs->fs_aextents, block, count,
                                  true);
                lc_markExtentsDirty(rfs);
            }
            fs->fs_blocks += count;

            /* Add unused blocks to the free reserve */
            if (count < rsize) {
                lc_addSpaceExtent(gfs, fs, &fs->fs_extents, block + count,
                                  rsize - count, false);
                fs->fs_reservedBlocks += rsize - count;
            }
        }
    }
    return block;
}

/* Flush extent pages */
static void
lc_flushExtentPages(struct gfs *gfs, struct fs *fs, struct page *fpage,
                    uint64_t pcount, uint64_t block) {
    struct page *page = fpage, *tpage;
    struct dextentBlock *eblock;
    uint64_t count = pcount;

    //lc_printf("Writing extents to block %ld count %ld\n", block, pcount);

    /* Link the blocks together */
    while (page) {
        count--;
        lc_addPageBlockHash(gfs, fs, page, block + count);
        eblock = (struct dextentBlock *)page->p_data;
        eblock->de_magic = LC_EXTENT_MAGIC;
        eblock->de_next = (page == fpage) ?
                          LC_INVALID_BLOCK : block + count + 1;
        lc_updateCRC(eblock, &eblock->de_crc);
        tpage = page;
        page = page->p_dnext;
    }
    assert(count == 0);
    lc_addPageForWriteBack(gfs, fs, fpage, tpage, pcount);
}

/* Free blocks used for storing allocated/free extent info */
static void
lc_freeExtentBlocks(struct gfs *gfs, struct fs *fs, uint64_t block,
                    uint64_t count, bool lock) {
    if (lock) {
        pthread_mutex_lock(&gfs->gfs_alock);
    }
    assert(gfs->gfs_super->sb_blocks >= count);
    gfs->gfs_super->sb_blocks -= count;
    lc_addSpaceExtent(gfs, fs, &gfs->gfs_fextents, block, count, true);
    if (lock) {
        pthread_mutex_unlock(&gfs->gfs_alock);
        lc_markExtentsDirty(fs);
    }
}

/* Perform requested actions on the extent list */
uint64_t
lc_blockFreeExtents(struct gfs *gfs, struct fs *fs, struct extent *extents,
                    uint8_t flags) {
    bool flush = flags & LC_EXTENT_FLUSH, efree = flags & LC_EXTENT_EFREE;
    uint64_t count = LC_EXTENT_BLOCK, pcount = 0, block, freed = 0;
    struct fs *rfs = flush ? lc_getGlobalFs(gfs) : NULL;
    struct extent *extent = extents, *tmp;
    bool free = !(flags & LC_EXTENT_KEEP);
    bool layer = flags & LC_EXTENT_LAYER;
    bool reuse = flags & LC_EXTENT_REUSE;
    struct dextentBlock *eblock = NULL;
    struct page *page = NULL;
    uint64_t estart, ecount;
    struct dextent *dextent;
    struct super *super;

    while (extent) {
        tmp = extent;
        assert(extent->ex_type == LC_EXTENT_SPACE);
        lc_validateExtent(gfs, extent);
        if (flush) {

            /* Add this extent to the disk block */
            if (count >= LC_EXTENT_BLOCK) {
                if (eblock) {
                    page = lc_getPageNoBlock(gfs, rfs, (char *)eblock, page);
                }
                lc_mallocBlockAligned(rfs, (void **)&eblock, LC_MEMTYPE_DATA);
                pcount++;
                count = 0;
            }
            dextent = &eblock->de_extents[count++];
            dextent->de_start = lc_getExtentStart(extent);
            dextent->de_count = lc_getExtentCount(extent);
        } else if (efree) {

            /* Free extent blocks */
            estart = lc_getExtentStart(extent);
            ecount = lc_getExtentCount(extent);
            freed += ecount;
            lc_blockFree(gfs, fs, estart, ecount, layer, reuse);
        }
        extent = extent->ex_next;

        /* Free the extent structure unless requested not to do so */
        if (free) {
            lc_free(fs, tmp, sizeof(struct extent), LC_MEMTYPE_EXTENT);
        }
    }

    /* Allocate page for the last block */
    if (eblock) {
        if (count < LC_EXTENT_BLOCK) {
            eblock->de_extents[count].de_start = 0;
        }
        page = lc_getPageNoBlock(gfs, rfs, (char *)eblock, page);
    }

    /* Write out the allocated/free extent info to disk */
    if (flush) {
        assert(pcount);
        if (layer) {
            super = fs->fs_super;
            if (super->sb_extentCount) {
                lc_freeExtentBlocks(gfs, rfs, super->sb_extentBlock,
                                    super->sb_extentCount, false);
            }

            /* Allocate a new block */
            block = lc_blockAllocExact(rfs, pcount, true, false);
            super->sb_extentBlock = block;
            super->sb_extentCount = pcount;
            lc_printf("Syncing allocated map layer %d block %ld count %ld\n",
                      fs->fs_gindex, block, pcount);
        } else {

            /* Use the pre-allocated block */
            block = gfs->gfs_super->sb_extentBlock;
            assert(block != LC_INVALID_BLOCK);
            lc_printf("Syncing free extent map to block %ld count %ld\n",
                      block, pcount);
        }

        /* Queue write of newly created pages */
        lc_flushExtentPages(gfs, rfs, page, pcount, block);
    }
    return freed;
}

/* Read extents list */
void
lc_readExtents(struct gfs *gfs, struct fs *fs) {
    uint64_t block, count = 0, ecount = 0;
    struct fs *rfs = lc_getGlobalFs(gfs);
    bool allocated = (fs != rfs);
    struct dextentBlock *eblock;
    struct dextent *dextent;
    struct extent **extents;
    int i;

    block = fs->fs_super->sb_extentBlock;
    if (block == LC_INVALID_BLOCK) {

        /* This could happen if layer crashed before unmounting */
        assert(fs->fs_gindex);
        assert(!fs->fs_frozen);
        assert(fs->fs_super->sb_flags & LC_SUPER_DIRTY);
        return;
    }
    extents = allocated ? &fs->fs_aextents : &gfs->gfs_extents;
    lc_mallocBlockAligned(fs, (void **)&eblock, LC_MEMTYPE_BLOCK);
    while (block != LC_INVALID_BLOCK) {
        //lc_printf("Reading extents from block %ld\n", block);
        /* Read and validate the block */
        lc_readBlock(gfs, fs, block, eblock);
        assert(eblock->de_magic == LC_EXTENT_MAGIC);
        lc_verifyBlock(eblock, &eblock->de_crc);

        /* Process extents in the block */
        for (i = 0; i < LC_EXTENT_BLOCK; i++) {
            dextent = &eblock->de_extents[i];
            if ((dextent->de_start == 0) || (dextent->de_count == 0)) {
                break;
            }
            lc_addSpaceExtent(gfs, fs, extents, dextent->de_start,
                              dextent->de_count, true);
            count += dextent->de_count;
        }
        block = eblock->de_next;
        ecount++;
    }
    assert((ecount == fs->fs_super->sb_extentCount) ||
           (!allocated && (ecount < fs->fs_super->sb_extentCount)));
    lc_free(fs, eblock, LC_BLOCK_SIZE, LC_MEMTYPE_BLOCK);
    if (allocated) {
        fs->fs_blocks = count;
        lc_printf("Total blocks in use in layer %ld\n", fs->fs_blocks);
    } else {
        lc_printf("Total free blocks %ld used blocks %ld total blocks %ld\n",
                  count, gfs->gfs_super->sb_blocks, gfs->gfs_super->sb_tblocks);
        assert((count + gfs->gfs_super->sb_blocks) ==
               gfs->gfs_super->sb_tblocks);
        gfs->gfs_blocksReserved = (gfs->gfs_super->sb_tblocks *
                                   LC_RESERVED_BLOCKS) / 100ul;
    }
}

/* Free blocks in the specified extent if it was allocated for the layer */
static uint64_t
lc_freeLayerExtent(struct gfs *gfs, struct fs *fs, struct fs *rfs,
                   uint64_t block, uint64_t count) {
    uint64_t freed, total = 0;

    assert(fs != rfs);

    /* Check if the extent was allocated for the layer */
    while (count) {

        /* Find the portion of the extent which is allocated in the layer */
        pthread_mutex_lock(&fs->fs_alock);
        freed = lc_removeExtent(fs, &fs->fs_aextents, block, count);
        pthread_mutex_unlock(&fs->fs_alock);
        if (freed) {
            lc_freeExtentBlocks(gfs, rfs, block, freed, true);
            total += freed;
        } else {

            /* Check next block if the previous block was not allocated in the
             * layer.
             */
            freed = 1;
        }
        block += freed;
        count -= freed;
    }
    return total;
}

/* If the block being freed is tracked in a allocated list of extents, free the
 * block after updating that list.  Otherwise, add the block to reserved pool.
 */
static void
lc_blockFreeLayer(struct gfs *gfs, struct fs *fs, struct fs *rfs,
                  uint64_t block, uint64_t count, bool reuse) {

    /* Extents allocated in a layer can be removed only after updating the
     * allocation block list.  If the extent is not part of that list, then the
     * extent was not allocated in the layer.
     */
    if (reuse) {
        pthread_mutex_lock(&fs->fs_alock);
        lc_addSpaceExtent(fs->fs_gfs, fs, &fs->fs_extents, block, count,
                          false);
        fs->fs_reservedBlocks += count;
        pthread_mutex_unlock(&fs->fs_alock);
    } else if (fs != rfs) {
        count = lc_freeLayerExtent(gfs, fs, rfs, block, count);
    } else {

        /* Release the blocks to the global pool */
        lc_freeExtentBlocks(gfs, rfs, block, count, true);
    }
    __sync_add_and_fetch(&fs->fs_freed, count);
}

/* Display allocation stats of the layer */
void
lc_displayAllocStats(struct fs *fs) {
    if (fs->fs_blocks) {
        lc_syslog(LOG_INFO, "\tblocks allocated %ld freed %ld in use %ld\n",
                  fs->fs_blocks, fs->fs_freed, fs->fs_blocks - fs->fs_freed);
    }
    if (fs->fs_reservedBlocks) {
        lc_syslog(LOG_INFO, "\tReserved blocks %ld\n", fs->fs_reservedBlocks);
    }
}

/* Allocate specified number of blocks */
uint64_t
lc_blockAlloc(struct fs *fs, uint64_t count, bool meta, bool reserve) {
    struct gfs *gfs = fs->fs_gfs;
    uint64_t block;

    pthread_mutex_lock(&fs->fs_alock);
    block = lc_findFreeBlock(gfs, fs, count, true, true);
    pthread_mutex_unlock(&fs->fs_alock);
    lc_markExtentsDirty(fs);
    assert(((block + count) < gfs->gfs_super->sb_tblocks) ||
           (block == LC_INVALID_BLOCK));
    return block;
}

/* Allocate specified number of blocks */
uint64_t
lc_blockAllocExact(struct fs *fs, uint64_t count, bool meta, bool reserve) {
    uint64_t block = lc_blockAlloc(fs, count, meta, reserve);

    assert(block != LC_INVALID_BLOCK);
    return block;
}

/* Free file system blocks */
void
lc_blockFree(struct gfs *gfs, struct fs *fs, uint64_t block,
             uint64_t count, bool layer, bool reuse) {
    struct fs *rfs = lc_getGlobalFs(gfs);

    assert(block && count);
    assert(block != LC_INVALID_BLOCK);
    assert((block + count) < gfs->gfs_super->sb_tblocks);
    if (!reuse) {
        lc_markExtentsDirty(fs);
    }
    if (layer) {

        /* Add blocks to the file system list for deferred processing */
        lc_blockFreeLayer(gfs, fs, rfs, block, count, reuse);
    } else {

        /* Add blocks back to the global free list */
        pthread_mutex_lock(&gfs->gfs_alock);
        lc_addSpaceExtent(gfs, rfs,
                          reuse ? &gfs->gfs_extents : &gfs->gfs_fextents,
                          block, count, true);
        assert(gfs->gfs_super->sb_blocks >= count);
        gfs->gfs_super->sb_blocks -= count;
        pthread_mutex_unlock(&gfs->gfs_alock);
        if (!reuse) {
            lc_markExtentsDirty(rfs);
        }
    }
}

/* Perform the requested operation on the list of extents allocated to the
 * layer.
 */
static void
lc_flushAllocatedExtents(struct gfs *gfs, struct fs *fs,
                         bool unmount, bool remove, bool flush) {
    struct extent *extent = fs->fs_aextents;
    int flags = flush ? LC_EXTENT_KEEP : 0;
    bool release = unmount || remove;
    uint64_t freed;

    if (remove) {
        flags |= LC_EXTENT_EFREE;
    } else if (fs->fs_extentsDirty) {
        flags |= LC_EXTENT_FLUSH | LC_EXTENT_LAYER;
        lc_markSuperDirty(fs);
    } else if (!unmount) {
        return;
    }
    if (release) {

        /* Reset this so that extents being freed will not be consulted with
         * this list again.
         */
        fs->fs_aextents = NULL;
    }
    freed = lc_blockFreeExtents(gfs, fs, extent, flags);
    if (release) {
        fs->fs_freed += freed;
    }
    fs->fs_extentsDirty = false;
}

/* Insert the list of extents to another */
static inline void
lc_moveExtents(struct fs *fs, struct extent **extents,
               struct extent *extent, bool empty) {
    struct extent *last = extent;

    if (empty) {
        assert(*extents == NULL);
        *extents = extent;
        return;
    }

    /* Find the last extent in the list */
    while (last && last->ex_next) {
        last = last->ex_next;
    }
    assert(last->ex_next == NULL);

    /* Link the provided list to list of the layer */
    if (fs) {
        pthread_mutex_lock(&fs->fs_alock);
    }
    last->ex_next = *extents;
    *extents = extent;
    if (fs) {
        pthread_mutex_unlock(&fs->fs_alock);
    }
}

/* Process blocks allocated/freed in a layer */
void
lc_processLayerBlocks(struct gfs *gfs, struct fs *fs, bool unmount,
                       bool remove, bool flush) {

    /* Release any unused reservation */
    lc_releaseReservedBlocks(gfs, fs);

    /* Process blocks freed in layer.  These blocks may or may not be
     * allocated in the layer
     */
    if (fs->fs_fextents) {
        lc_blockFreeExtents(gfs, fs, fs->fs_fextents,
                            remove ? 0 : (LC_EXTENT_EFREE | LC_EXTENT_LAYER));
        fs->fs_fextents = NULL;
    }

    /* If the layer is being removed, then free any blocks allocated in the
     * layer, otherwise free the list after writing that to disk.
     */
    if (fs->fs_aextents) {
        assert(fs != lc_getGlobalFs(gfs));
        lc_flushAllocatedExtents(gfs, fs, unmount, remove, flush);
    }
}

/* Track an extent freed from a layer */
void
lc_addFreedBlocks(struct fs *fs, uint64_t block, uint64_t count) {
    pthread_mutex_lock(&fs->fs_alock);
    lc_addSpaceExtent(fs->fs_gfs, fs, &fs->fs_fextents, block, count, false);
    pthread_mutex_unlock(&fs->fs_alock);
}

/* Track a list of extents freed from a layer */
void
lc_addFreedExtents(struct fs *fs, struct extent *extent, bool empty) {
    lc_moveExtents(fs, &fs->fs_fextents, extent, empty);
}

/* Replace extents in the specified list with new extent provided.
 * Old extents will be moved to freed list
 */
void
lc_replaceFreedExtents(struct fs *fs, struct extent **extents,
                       uint64_t block, uint64_t count) {
    struct extent *extent = *extents, *tmp;
    struct gfs *gfs = fs->fs_gfs;
    bool insert = true;

    assert((block + count) < gfs->gfs_super->sb_tblocks);
    while (extent) {
        assert(extent->ex_type == LC_EXTENT_SPACE);
        lc_validateExtent(gfs, extent);

        /* Free blocks covered by this extent */
        lc_addFreedBlocks(fs, lc_getExtentStart(extent),
                          lc_getExtentCount(extent));
        if (insert) {

            /* Use the same extent to track new blocks */
            lc_initExtent(NULL, extent, LC_EXTENT_SPACE,
                          block, 0, count, NULL);
            tmp = extent->ex_next;
            insert = false;
            extent = tmp;
        } else {
            assert(insert);

            /* Free the extent */
            tmp = extent;
            extent = extent->ex_next;
            lc_free(fs, tmp, sizeof(struct extent), LC_MEMTYPE_EXTENT);
        }
    }

    /* Allocate a new extent and track the new blocks in it */
    if (insert) {
        assert((*extents) == NULL);
        lc_addSpaceExtent(fs->fs_gfs, fs, extents, block, count, false);
    }
}

/* Count number of extents in the list */
uint64_t
lc_countExtents(struct gfs *gfs, struct extent *extent, uint64_t *bcount) {
    uint64_t count = 0, total = 0;

    while (extent) {
        assert(extent->ex_type == LC_EXTENT_SPACE);
        lc_validateExtent(gfs, extent);
        count++;
        if (bcount) {
            total += lc_getExtentCount(extent);
        }
        extent = extent->ex_next;
    }
    if (bcount) {
        *bcount += total;
    }
    return count;
}

/* Flush and/or release global list of free extents to disk */
void
lc_processFreeExtents(struct gfs *gfs, struct fs *fs, bool umount) {
    uint64_t count, pcount, block = LC_INVALID_BLOCK, bcount = 0;
    bool release, flush = fs->fs_extentsDirty;
    struct extent *extent, **prev;

    if (flush) {

        /* Count the number of free extents to find number of blocks needed */
        count = lc_countExtents(gfs, gfs->gfs_extents, &bcount);
        count += lc_countExtents(gfs, gfs->gfs_fextents, &bcount);
        pcount = (count + LC_EXTENT_BLOCK - 1) / LC_EXTENT_BLOCK;
        assert(pcount);

        /* Allocate blocks for storing free space extents */
        /* XXX Make sure space exists for tracking free space extents */
        prev = &gfs->gfs_extents;
        extent = gfs->gfs_extents;
        while (extent) {
            if (lc_getExtentCount(extent) >= pcount) {
                block = lc_getExtentStart(extent);
                lc_incrExtentStart(NULL, extent, pcount);
                release = lc_decrExtentCount(gfs, extent, pcount);
                if (release) {
                    lc_freeExtent(gfs, fs, extent, prev, true);
                }
                break;
            }
            prev = &extent->ex_next;
            extent = extent->ex_next;
        }
        assert(block != LC_INVALID_BLOCK);
        assert((block + pcount) < gfs->gfs_super->sb_tblocks);
        gfs->gfs_super->sb_blocks += pcount;
        bcount -= pcount;
        assert((bcount + gfs->gfs_super->sb_blocks) ==
               gfs->gfs_super->sb_tblocks);

        if (gfs->gfs_super->sb_extentCount) {
            lc_freeExtentBlocks(gfs, fs, gfs->gfs_super->sb_extentBlock,
                                gfs->gfs_super->sb_extentCount, false);
        }
        gfs->gfs_super->sb_extentBlock = block;
        gfs->gfs_super->sb_extentCount = pcount;
    } else {
        assert(gfs->gfs_fextents == NULL);
    }

    /* Transfer all the extents freed so far */
    prev = &gfs->gfs_fextents;
    extent = gfs->gfs_fextents;
    while (extent) {
        lc_addSpaceExtent(gfs, fs, &gfs->gfs_extents,
                          lc_getExtentStart(extent), lc_getExtentCount(extent),
                          true);
        *prev = extent->ex_next;
        lc_free(fs, extent, sizeof(struct extent), LC_MEMTYPE_EXTENT);
        extent = *prev;
    }

    /* Flush global list of free extents to disk */
    lc_blockFreeExtents(gfs, fs, gfs->gfs_extents,
                        (umount ? 0 : LC_EXTENT_KEEP) |
                        (flush ? LC_EXTENT_FLUSH : 0));
    if (umount) {
        gfs->gfs_extents = NULL;
    }
    if (flush) {
        fs->fs_extentsDirty = false;
        lc_markSuperDirty(fs);
    }
}
