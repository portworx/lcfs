#include "includes.h"

/* Space reserved as a percentage.  When available free space reaches this
 * threshold new writes and new layers are failed.
 */
#define LC_RESERVED_BLOCKS  10ul

/* Number of blocks reserved by a layer for allocating for metadata.  This
 * helps to avoid fragmentation.
 */
#define LC_META_RESERVE     1024

/* Number of blocks reserved by a layer from the global pool */
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
                  0, gfs->gfs_super->sb_tblocks - LC_START_BLOCK - 1, NULL);
    gfs->gfs_extents = extent;
    gfs->gfs_blocksReserved = (gfs->gfs_super->sb_tblocks *
                               LC_RESERVED_BLOCKS) / 100ul;
}

/* Reclaim reserved space from all layers */
static uint64_t
lc_reclaimSpace(struct gfs *gfs) {
    uint8_t flags = LC_EXTENT_EFREE | LC_EXTENT_LAYER;
    struct extent *extent;
    uint64_t count = 0;
    struct fs *fs;
    int i;

    rcu_register_thread();
    rcu_read_lock();
    for (i = 0; i <= gfs->gfs_scount; i++) {
        fs = rcu_dereference(gfs->gfs_fs[i]);

        /* Locking a layer would fail only when the layer is being deleted */
        if (fs &&
            (fs->fs_extents || fs->fs_blockInodesCount ||
             fs->fs_blockMetaCount || fs->fs_fextents ||
             fs->fs_mextents || fs->fs_dextents) && !lc_tryLock(fs, false)) {
            rcu_read_unlock();

            /* Flush dirty pages so that freed blocks can be released */
            if (fs->fs_dextents) {
                lc_flushDirtyPages(gfs, fs);
                lc_freeBlocksAfterFlush(fs, 0);
            }

            /* Release freed blocks */
            if (fs->fs_fextents || fs->fs_mextents) {
                pthread_mutex_lock(&fs->fs_alock);
                if (fs->fs_fextents) {
                    extent = fs->fs_fextents;
                    fs->fs_fextents = NULL;
                    pthread_mutex_unlock(&fs->fs_alock);
                    lc_blockFreeExtents(gfs, fs, extent, flags);
                    pthread_mutex_lock(&fs->fs_alock);
                }
                if (fs->fs_mextents) {
                    extent = fs->fs_mextents;
                    fs->fs_mextents = NULL;
                    pthread_mutex_unlock(&fs->fs_alock);
                    lc_blockFreeExtents(gfs, fs, extent, flags);
                } else {
                    pthread_mutex_unlock(&fs->fs_alock);
                }
            }

            /* Release any reserved blocks */
            if (fs->fs_extents || fs->fs_blockInodesCount ||
                fs->fs_blockMetaCount) {
                count += lc_freeLayerBlocks(gfs, fs, false, false, false);
            }
            lc_unlock(fs);
            rcu_read_lock();
            if (count >= LC_RECLAIM_BLOCKS) {
                break;
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
    lc_addExtent(gfs, fs, extents, start, 0, count, sort);
}

/* Find a run of free blocks from the free extent list */
static uint64_t
lc_findFreeBlock(struct gfs *gfs, struct fs *fs,
                 uint64_t count, bool reserve, bool layer) {
    uint64_t block = LC_INVALID_BLOCK, rsize;
    struct extent **extents, *extent, **prev;
    bool release;

    /* Check if an extent with enough free blocks available */
    extents = layer ? &fs->fs_extents : &gfs->gfs_extents;
    prev = extents;
    extent = *extents;
    while (extent) {
        if (lc_getExtentCount(extent) >= count) {
            block = lc_getExtentStart(extent);
            lc_incrExtentStart(NULL, extent, count);
            release = lc_decrExtentCount(gfs, extent, count);

            /* Free the extent if it is fully consumed */
            if (release) {
                lc_freeExtent(gfs, fs, extent, prev, layer);
            }

            if (!layer) {

                /* Update global usage */
                gfs->gfs_super->sb_blocks += count;
                assert(gfs->gfs_super->sb_tblocks > gfs->gfs_super->sb_blocks);
            } else {
                assert(fs->fs_reservedBlocks >= count);
                fs->fs_reservedBlocks -= count;
                if (fs != lc_getGlobalFs(gfs)) {

                    /* Track allocated extents for a layer */
                    lc_addSpaceExtent(gfs, fs, &fs->fs_aextents, block, count,
                                      true);
                    fs->fs_blocks += count;
                }
            }
            assert(block < gfs->gfs_super->sb_tblocks);
            return block;
        }
        prev = &extent->ex_next;
        extent = extent->ex_next;
    }

    /* If the layer does not have any reserved chunks, get one */
    if (layer) {
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
            if (fs != lc_getGlobalFs(gfs)) {

                /* Track the allocated space for the layer */
                lc_addSpaceExtent(gfs, fs, &fs->fs_aextents, block, count,
                                  true);
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
    struct dextentBlock *eblock;
    struct page *page = fpage;
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
        page = page->p_dnext;
    }
    assert(count == 0);
    lc_flushPageCluster(gfs, fs, fpage, pcount, false);
}

/* Free an extent list, and optionally updating the list on disk */
uint64_t
lc_blockFreeExtents(struct gfs *gfs, struct fs *fs, struct extent *extents,
                    uint8_t flags) {
    bool flush = flags & LC_EXTENT_FLUSH, efree = flags & LC_EXTENT_EFREE;
    uint64_t count = LC_EXTENT_BLOCK, pcount = 0, block, freed = 0;
    struct fs *rfs = flush ? lc_getGlobalFs(gfs) : NULL;
    struct extent *extent = extents, *tmp;
    bool layer = flags & LC_EXTENT_LAYER;
    uint64_t estart, ecount, bcount = 0;
    struct dextentBlock *eblock = NULL;
    struct page *page = NULL;
    struct dextent *dextent;

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
            lc_blockFree(gfs, fs, estart, ecount, layer);
        }
        extent = extent->ex_next;
        lc_free(fs, tmp, sizeof(struct extent), LC_MEMTYPE_EXTENT);
    }
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

            /* Allocate a new block */
            block = lc_blockAllocExact(rfs, pcount, true, false);
            fs->fs_super->sb_extentBlock = block;
        } else {
            block = gfs->gfs_super->sb_extentBlock;
            assert(block != LC_INVALID_BLOCK);
        }
        lc_flushExtentPages(gfs, rfs, page, pcount, block);
    } else if (bcount) {
        __sync_add_and_fetch(&gfs->gfs_preused, bcount);
    }
    return freed;
}

/* Read extents list */
void
lc_readExtents(struct gfs *gfs, struct fs *fs) {
    struct fs *rfs = lc_getGlobalFs(gfs);
    struct dextentBlock *eblock;
    uint64_t block, count = 0;
    struct dextent *dextent;
    struct extent **extents;
    bool allocated;
    int i;

    block = fs->fs_super->sb_extentBlock;
    assert(block != LC_INVALID_BLOCK);
    allocated = (fs != lc_getGlobalFs(gfs));
    extents = allocated ? &fs->fs_aextents : &gfs->gfs_extents;
    lc_mallocBlockAligned(fs, (void **)&eblock, LC_MEMTYPE_BLOCK);
    while (block != LC_INVALID_BLOCK) {
        //lc_printf("Reading extents from block %ld\n", block);
        lc_addSpaceExtent(gfs, rfs, &fs->fs_dextents, block, 1, false);
        lc_readBlock(gfs, fs, block, eblock);
        lc_verifyBlock(eblock, &eblock->de_crc);
        assert(eblock->de_magic == LC_EXTENT_MAGIC);

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
    }
    lc_free(fs, eblock, LC_BLOCK_SIZE, LC_MEMTYPE_BLOCK);
    if (allocated) {
        fs->fs_blocks = count;
        lc_printf("Total blocks in use in layer %ld\n", fs->fs_blocks);
    } else {
        lc_printf("Total free blocks %ld used blocks %ld total blocks %ld\n",
                  count, gfs->gfs_super->sb_blocks, gfs->gfs_super->sb_tblocks);
        assert((count + gfs->gfs_super->sb_blocks + 1) ==
               gfs->gfs_super->sb_tblocks);
        gfs->gfs_blocksReserved = (gfs->gfs_super->sb_tblocks *
                                   LC_RESERVED_BLOCKS) / 100ul;
    }
}

/* Free blocks in the specified extent if it was allocated for the layer */
static void
lc_freeLayerExtent(struct fs *fs, uint64_t block, uint64_t count) {
    uint64_t freed;

    /* Check if the extent was allocated for the layer */
    while (count) {
        freed = lc_removeExtent(fs, &fs->fs_aextents, block, count);
        if (freed) {

            /* Free the blocks which were allocated in the layer */
            lc_addSpaceExtent(fs->fs_gfs, fs, &fs->fs_extents, block, freed,
                              false);
            fs->fs_freed += freed;
            fs->fs_reservedBlocks += freed;
        } else {

            /* Check next block if the previous block was not allocated in the
             * layer.
             */
            freed = 1;
        }
        block += freed;
        count -= freed;
    }
}

/* Add the extent to free list for reuse or deferred processing */
static void
lc_blockLayerFree(struct gfs *gfs, struct fs *fs, uint64_t block,
                  uint64_t count) {
    if ((fs != lc_getGlobalFs(gfs)) && (fs->fs_aextents != NULL)) {

        /* If the layer does not track allocations, free the blocks */
        lc_freeLayerExtent(fs, block, count);
    } else {

        /* Add the blocks to reserve pool of the layer for now */
        lc_addSpaceExtent(fs->fs_gfs, fs, &fs->fs_extents, block, count,
                          false);
        fs->fs_reservedBlocks += count;
    }
}

/* Display allocation stats of the layer */
void
lc_displayAllocStats(struct fs *fs) {
    if (fs->fs_blocks) {
        printf("\tblocks allocated %ld freed %ld in use %ld\n",
               fs->fs_blocks, fs->fs_freed, fs->fs_blocks - fs->fs_freed);
    }
    if (fs->fs_reservedBlocks || fs->fs_blockMetaCount ||
        fs->fs_blockInodesCount) {
        printf("\tReserved blocks %ld Metablocks %ld Inode Blocks %ld\n",
               fs->fs_reservedBlocks, fs->fs_blockMetaCount,
               fs->fs_blockInodesCount);
    }
}

/* Allocate specified number of blocks */
uint64_t
lc_blockAlloc(struct fs *fs, uint64_t count, bool meta, bool reserve) {
    struct gfs *gfs = fs->fs_gfs;
    uint64_t block;

    pthread_mutex_lock(&fs->fs_alock);
    if (meta) {

        /* Check if space available in the reserve pool.  If not, try to get
         * some.
         */
        if (fs->fs_blockMetaCount < count) {

            /* Release previous reservation */
            if (fs->fs_blockMetaCount) {
                lc_blockLayerFree(gfs, fs, fs->fs_blockMeta,
                                  fs->fs_blockMetaCount);
            }

            /* Try to make a larger reservation */
            fs->fs_blockMetaCount = (!reserve || count > LC_META_RESERVE) ?
                                    count : LC_META_RESERVE;
            fs->fs_blockMeta = lc_findFreeBlock(gfs, fs,
                                                fs->fs_blockMetaCount,
                                                reserve, true);

            /* Retry without reservation */
            if ((fs->fs_blockMeta == LC_INVALID_BLOCK) &&
                (count < fs->fs_blockMetaCount)) {
                fs->fs_blockMetaCount = count;
                fs->fs_blockMeta = lc_findFreeBlock(gfs, fs,
                                                    fs->fs_blockMetaCount,
                                                    false, true);
                if (fs->fs_blockMeta == LC_INVALID_BLOCK) {
                    fs->fs_blockMetaCount = 0;
                    pthread_mutex_unlock(&fs->fs_alock);
                    return LC_INVALID_BLOCK;
                }
            }
        }

        /* Make the allocation */
        assert(fs->fs_blockMeta != LC_INVALID_BLOCK);
        assert(fs->fs_blockMetaCount >= count);
        block = fs->fs_blockMeta;
        fs->fs_blockMeta += count;
        fs->fs_blockMetaCount -= count;
    } else {

        /* Allocation for regular data */
        block = lc_findFreeBlock(gfs, fs, count, true, true);

        /* If allocation failed, but space available in metadata reservation,
         * use that.
         */
        if ((block == LC_INVALID_BLOCK) && (count == 1) &&
            fs->fs_blockMetaCount) {
            block = fs->fs_blockMeta;
            fs->fs_blockMeta += count;
            fs->fs_blockMetaCount -= count;
        }
    }
    pthread_mutex_unlock(&fs->fs_alock);
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
             uint64_t count, bool layer) {
    struct fs *rfs;

    assert(block && count);
    assert(block != LC_INVALID_BLOCK);
    assert((block + count) < gfs->gfs_super->sb_tblocks);
    if (layer) {

        /* Add blocks to the file system list for deferred processing */
        pthread_mutex_lock(&fs->fs_alock);
        lc_blockLayerFree(gfs, fs, block, count);
        pthread_mutex_unlock(&fs->fs_alock);
    } else {
        rfs = lc_getGlobalFs(gfs);

        /* Add blocks back to the global free list */
        pthread_mutex_lock(&gfs->gfs_alock);
        lc_addSpaceExtent(gfs, rfs, &gfs->gfs_extents, block, count, true);
        assert(gfs->gfs_super->sb_blocks >= count);
        gfs->gfs_super->sb_blocks -= count;
        pthread_mutex_unlock(&gfs->gfs_alock);
    }
}

/* Free blocks allocated/reserved by a layer */
uint64_t
lc_freeLayerBlocks(struct gfs *gfs, struct fs *fs, bool unmount, bool remove,
                   bool inval) {
    struct extent *extent;
    uint64_t freed;

    /* Free unused blocks from the inode pool */
    pthread_mutex_lock(&fs->fs_alock);
    if (fs->fs_blockInodesCount) {
        lc_blockLayerFree(gfs, fs, fs->fs_blockInodes,
                          fs->fs_blockInodesCount);
        fs->fs_blockInodesCount = 0;
        fs->fs_blockInodes = 0;
        fs->fs_inodeBlockIndex = 0;
    }

    /* Free unused blocks from the metadata pool */
    if (fs->fs_blockMetaCount) {
        lc_blockLayerFree(gfs, fs, fs->fs_blockMeta, fs->fs_blockMetaCount);
        fs->fs_blockMetaCount = 0;
        fs->fs_blockMeta = 0;
    }

    /* If the layer is being removed, then free any blocks allocated in the
     * layer, otherwise free the list after writing that to disk.
     */
    extent = fs->fs_aextents;
    if (unmount && extent) {
        fs->fs_aextents = NULL;
        assert(fs != lc_getGlobalFs(gfs));
        fs->fs_freed += lc_blockFreeExtents(gfs, fs, extent,
                                            remove ? LC_EXTENT_EFREE :
                                            (LC_EXTENT_FLUSH |
                                             LC_EXTENT_LAYER));

        /* Free blocks used for allocation extents earlier */
        if (fs->fs_dextents) {
            lc_blockFreeExtents(gfs, lc_getGlobalFs(gfs), fs->fs_dextents,
                                LC_EXTENT_EFREE | LC_EXTENT_LAYER);
            fs->fs_dextents = NULL;
        }
    }

    /* Release any unused reserved blocks */
    freed = lc_blockFreeExtents(gfs, fs, fs->fs_extents, LC_EXTENT_EFREE);
    assert(fs->fs_reservedBlocks == freed);
    fs->fs_reservedBlocks -= freed;
    fs->fs_extents = NULL;
    pthread_mutex_unlock(&fs->fs_alock);
    return freed;
}

/* Data extent for pending removal */
void
lc_freeLayerDataBlocks(struct fs *fs, uint64_t block, uint64_t count,
                       bool allocated) {

    pthread_mutex_lock(&fs->fs_alock);
    assert(allocated || (fs != lc_getGlobalFs(fs->fs_gfs)));
    lc_addSpaceExtent(fs->fs_gfs, fs,
                 allocated ? &fs->fs_fdextents : &fs->fs_fextents,
                 block, count, false);
    pthread_mutex_unlock(&fs->fs_alock);
}

/* Metadata extent for pending removal */
void
lc_freeLayerMetaBlocks(struct fs *fs, uint64_t block, uint64_t count) {
    pthread_mutex_lock(&fs->fs_alock);
    lc_addSpaceExtent(fs->fs_gfs, fs, &fs->fs_mextents, block, count, false);
    pthread_mutex_unlock(&fs->fs_alock);
}

/* Replace the metadata list with the new extent */
void
lc_replaceMetaBlocks(struct fs *fs, struct extent **extents,
                     uint64_t block, uint64_t count) {
    struct extent *extent = *extents, *tmp;
    struct gfs *gfs = fs->fs_gfs;
    bool insert = true;

    assert((block + count) < gfs->gfs_super->sb_tblocks);
    while (extent) {
        assert(extent->ex_type == LC_EXTENT_SPACE);
        lc_validateExtent(gfs, extent);

        /* Free blocks covered by this extent */
        lc_freeLayerMetaBlocks(fs, lc_getExtentStart(extent),
                               lc_getExtentCount(extent));
        if (insert) {

            /* Use the same extent to track new blocks */
            lc_initExtent(NULL, extent, LC_EXTENT_SPACE,
                          block, 0, count, NULL);
            tmp = extent->ex_next;
            insert = false;
            extent = tmp;
        } else {

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

/* Free blocks allocated and freed in a layer */
void
lc_processFreedBlocks(struct fs *fs, bool release) {
    uint8_t flags = release ? (LC_EXTENT_EFREE | LC_EXTENT_LAYER) : 0;
    struct gfs *gfs = fs->fs_gfs;

    /* These blocks may or may not be allocated for the layer */
    if (fs->fs_fextents) {
        lc_blockFreeExtents(gfs, fs, fs->fs_fextents, flags);
        fs->fs_fextents = NULL;
    }

    /* These are blocks allocated and freed for the layer */
    if (fs->fs_fdextents) {
        lc_blockFreeExtents(gfs, fs, fs->fs_fdextents, flags);
        fs->fs_fdextents = NULL;
    }

    /* Metadata blocks freed from the layer */
    if (fs->fs_mextents) {
        lc_blockFreeExtents(gfs, fs, fs->fs_mextents, flags);
        fs->fs_mextents = NULL;
    }
}

/* Update free space information to disk and free extent list */
void
lc_blockAllocatorDeinit(struct gfs *gfs, struct fs *fs) {
    uint64_t count = 0, pcount, block = LC_INVALID_BLOCK, bcount = 0;
    struct extent *extent, **prev;
    bool release;

    /* Free previously used blocks for storing free extent info */
    lc_blockFreeExtents(gfs, fs, fs->fs_dextents, LC_EXTENT_EFREE);
    fs->fs_dextents = NULL;

    /* Count the number of free extents to find number of blocks needed */
    extent = gfs->gfs_extents;
    while (extent) {
        assert(extent->ex_type == LC_EXTENT_SPACE);
        lc_validateExtent(gfs, extent);
        count++;
        bcount += lc_getExtentCount(extent);
        extent = extent->ex_next;
    }
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
    assert((bcount + gfs->gfs_super->sb_blocks + 1) ==
           gfs->gfs_super->sb_tblocks);
    gfs->gfs_super->sb_extentBlock = block;

    /* Update space usage */
    lc_blockFreeExtents(gfs, fs, gfs->gfs_extents,
                        fs->fs_dirty ? LC_EXTENT_FLUSH : 0);
    gfs->gfs_extents = NULL;
}
