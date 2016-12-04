#include "includes.h"

#define LC_RESERVED_BLOCKS  10ul
#define LC_META_RESERVE     1024
#define LC_BLOCK_RESERVE    8192

/* Initializes the block allocator */
void
lc_blockAllocatorInit(struct gfs *gfs) {
    struct extent *extent;

    extent = malloc(sizeof(struct extent));
    extent->ex_start = LC_START_BLOCK;
    extent->ex_count = gfs->gfs_super->sb_tblocks - LC_START_BLOCK - 1;
    extent->ex_next = NULL;
    gfs->gfs_extents = extent;
    gfs->gfs_blocksReserved = (gfs->gfs_super->sb_tblocks *
                               LC_RESERVED_BLOCKS) / 100ul;
}

/* Check if file system has enough space for the operation to proceed */
bool
lc_hasSpace(struct gfs *gfs, uint64_t blocks) {
    return gfs->gfs_super->sb_tblocks >
           (gfs->gfs_super->sb_blocks + gfs->gfs_blocksReserved + blocks);
}

/* Merge nearby extents */
static void
lc_mergeExtents(struct gfs *gfs, struct extent *extent, struct extent *prev) {
    struct extent *next = extent->ex_next;

    if (next && ((extent->ex_start + extent->ex_count) == next->ex_start)) {
        extent->ex_count += next->ex_count;
        extent->ex_next = next->ex_next;
        assert((extent->ex_start + extent->ex_count) <
               gfs->gfs_super->sb_tblocks);
        free(next);
    }
    if (prev && ((prev->ex_start + prev->ex_count) == extent->ex_start)) {
        prev->ex_count += extent->ex_count;
        prev->ex_next = extent->ex_next;
        assert((prev->ex_start + prev->ex_count) <
               gfs->gfs_super->sb_tblocks);
        free(extent);
    }
}

/* Add an extent to an extent list */
void
lc_addExtent(struct gfs *gfs, struct extent **extents,
             uint64_t block, uint64_t count) {
    struct extent *extent = *extents, *prev = NULL, *new;

    /* Look if the freed blocks could be merged to an existing extent */
    assert((block + count) < gfs->gfs_super->sb_tblocks);
    while (extent) {
        if ((extent->ex_start + extent->ex_count) == block) {
            extent->ex_count += count;
            assert((extent->ex_start + extent->ex_count) <
                   gfs->gfs_super->sb_tblocks);
            lc_mergeExtents(gfs, extent, NULL);
            return;
        }
        if ((block + count) == extent->ex_start) {
            extent->ex_start -= count;
            extent->ex_count += count;
            assert((extent->ex_start + extent->ex_count) <
                   gfs->gfs_super->sb_tblocks);
            lc_mergeExtents(gfs, extent, prev);
            return;
        }
        if (block < extent->ex_start) {
            break;
        }
        assert(block > (extent->ex_start + extent->ex_count));
        prev = extent;
        extent = extent->ex_next;
    }

    /* Need to add a new extent */
    new = malloc(sizeof(struct extent));
    new->ex_start = block;
    new->ex_count = count;
    new->ex_next = extent;
    if (prev == NULL) {
        *extents = new;
    } else {
        prev->ex_next = new;
    }
}

/* Find a run of free blocks from the free extent list */
static uint64_t
lc_findFreeBlock(struct gfs *gfs, struct fs *fs,
                 uint64_t count, bool reserve) {
    uint64_t block = LC_INVALID_BLOCK, rsize;
    struct extent **extents, *extent, *prev = NULL;

    /* Check if an extent with enough free blocks available */
    extents = fs ? &fs->fs_extents : &gfs->gfs_extents;
    extent = *extents;
    while (extent) {
        if (extent->ex_count >= count) {
            block = extent->ex_start;
            extent->ex_start += count;
            extent->ex_count -= count;
            assert((extent->ex_start + extent->ex_count) <
                   gfs->gfs_super->sb_tblocks);

            /* Free the extent if it is fully consumed */
            if (extent->ex_count == 0) {
                if (prev == NULL) {
                    *extents = extent->ex_next;
                } else {
                    prev->ex_next = extent->ex_next;
                }
                free(extent);
            }

            if (fs == NULL) {

                /* Update global usage */
                gfs->gfs_super->sb_blocks += count;
            } else if (fs != lc_getGlobalFs(gfs)) {

                /* Track allocated extents for a layer */
                lc_addExtent(gfs, &fs->fs_aextents, block, count);
            }
            assert(block < gfs->gfs_super->sb_tblocks);
            return block;
        }
        prev = extent;
        extent = extent->ex_next;
    }

    /* If the layer does not have any reserved chunks, make one */
    if (fs) {
        rsize = (!reserve || (count > LC_BLOCK_RESERVE)) ?
                count : LC_BLOCK_RESERVE;
        pthread_mutex_lock(&gfs->gfs_alock);
        block = lc_findFreeBlock(gfs, NULL, rsize, false);
        if ((block == LC_INVALID_BLOCK) && (count < rsize)) {
            rsize = count;
            block = lc_findFreeBlock(gfs, NULL, rsize, false);
        }
        pthread_mutex_unlock(&gfs->gfs_alock);
        if (block != LC_INVALID_BLOCK) {
            if (fs != lc_getGlobalFs(gfs)) {
                lc_addExtent(gfs, &fs->fs_aextents, block, count);
            }

            /* Add unused blocks to the free reserve */
            if (count < rsize) {
                lc_addExtent(gfs, &fs->fs_extents, block + count,
                             rsize - count);
            }
            fs->fs_blocks += rsize;
        }
    }
    return block;
}

/* Flush extent pages */
static void
lc_flushExtentPages(struct gfs *gfs, struct page *fpage, uint64_t pcount,
                    uint64_t block) {
    struct dextentBlock *eblock;
    struct page *page = fpage;
    uint64_t count = pcount;
    struct fs *fs;

    //lc_printf("Writing extents to block %ld count %ld\n", block, pcount);
    fs = lc_getGlobalFs(gfs);
    while (page) {
        count--;
        lc_addPageBlockHash(gfs, fs, page, block + count);
        eblock = (struct dextentBlock *)page->p_data;
        eblock->de_next = (page == fpage) ?
                          LC_INVALID_BLOCK : block + count + 1;
        page = page->p_dnext;
    }
    assert(count == 0);
    lc_flushPageCluster(gfs, fs, fpage, pcount);
}

/* Free an extent list, and optionally updating the list on disk */
uint64_t
lc_blockFreeExtents(struct fs *fs, struct extent *extents,
                    bool efree, bool flush) {
    uint64_t count = LC_EXTENT_BLOCK, pcount = 0, block, freed = 0;
    struct extent *extent = extents, *tmp;
    struct dextentBlock *eblock = NULL;
    struct gfs *gfs = getfs();
    struct page *page = NULL;
    struct dextent *dextent;

    while (extent) {
        tmp = extent;
        if (flush && extent->ex_count) {
            assert((extent->ex_start + extent->ex_count) <
                   gfs->gfs_super->sb_tblocks);
            if (count >= LC_EXTENT_BLOCK) {
                if (eblock) {
                    page = lc_getPageNoBlock(gfs, fs, (char *)eblock, page);
                }
                malloc_aligned((void **)&eblock);
                pcount++;
                count = 0;
            }
            dextent = &eblock->de_extents[count++];
            dextent->de_start = extent->ex_start;
            dextent->de_count = extent->ex_count;
        } else if (efree && extent->ex_count) {
            freed += extent->ex_count;
            lc_blockFree(fs, extent->ex_start, extent->ex_count);
        }
        extent = extent->ex_next;
        free(tmp);
    }
    if (eblock) {
        if (count < LC_EXTENT_BLOCK) {
            eblock->de_extents[count].de_start = 0;
        }
        page = lc_getPageNoBlock(gfs, fs, (char *)eblock, page);
    }

    /* Write out the allocated/free extent info to disk */
    if (flush) {
        assert(pcount);
        if (fs) {
            block = lc_blockAllocExact(lc_getGlobalFs(gfs), pcount,
                                       true, false);
            fs->fs_super->sb_extentBlock = block;
        } else {
            block = gfs->gfs_super->sb_extentBlock;
            assert(block != LC_INVALID_BLOCK);
        }
        lc_flushExtentPages(gfs, page, pcount, block);
    }
    return freed;
}

/* Read extents list */
void
lc_readExtents(struct gfs *gfs, struct fs *fs) {
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
    malloc_aligned((void **)&eblock);
    while (block != LC_INVALID_BLOCK) {
        //lc_printf("Reading extents from block %ld\n", block);
        lc_addExtent(gfs, &fs->fs_dextents, block, 1);
        lc_readBlock(gfs, fs, block, eblock);
        for (i = 0; i < LC_EXTENT_BLOCK; i++) {
            dextent = &eblock->de_extents[i];
            if ((dextent->de_start == 0) || (dextent->de_count == 0)) {
                break;
            }
            lc_addExtent(gfs, extents, dextent->de_start,
                         dextent->de_count);
            count += dextent->de_count;
        }
        block = eblock->de_next;
    }
    free(eblock);
    if (allocated) {
        fs->fs_blocks = count;
        lc_printf("Total blocks in use in layer %ld\n", fs->fs_blocks);
    } else {
        lc_printf("Total free blocks %ld used blocks %ld total blocks %ld\n",
                  count, gfs->gfs_super->sb_blocks, gfs->gfs_super->sb_tblocks);
        assert((count + gfs->gfs_super->sb_blocks + 1) ==
               gfs->gfs_super->sb_tblocks);
    }
}

/* Update allocated extent list with the extent freed */
static struct extent *
lc_updateAllocList(struct fs *fs, struct extent *extent,
                   uint64_t block, uint64_t freed) {
    struct gfs *gfs = fs->fs_gfs;
    struct extent *new;

    assert(extent->ex_count >= freed);
    assert(block >= extent->ex_start);
    assert((block + freed) <= (extent->ex_start + extent->ex_count));
    assert(freed <= extent->ex_count);
    if (extent->ex_start == block) {
        extent->ex_start += freed;
        extent->ex_count -= freed;
        assert((extent->ex_start + extent->ex_count) <
               gfs->gfs_super->sb_tblocks);
    } else if ((block + freed) ==
               (extent->ex_start + extent->ex_count)) {
        extent->ex_count -= freed;
        assert((extent->ex_start + extent->ex_count) <
               gfs->gfs_super->sb_tblocks);
    } else {

        /* Split the extent */
        new = malloc(sizeof(struct extent));
        new->ex_start = block + freed;
        new->ex_count = (extent->ex_start + extent->ex_count) -
                        new->ex_start;
        assert(new->ex_count > 0);
        assert((new->ex_start + new->ex_count) < gfs->gfs_super->sb_tblocks);
        extent->ex_count = block - extent->ex_start;
        assert((extent->ex_start + extent->ex_count) <
               gfs->gfs_super->sb_tblocks);
        assert(extent->ex_count > 0);
        new->ex_next = extent->ex_next;
        extent->ex_next = new;
    }

    if ((extent->ex_count == 0) && (fs->fs_aextents == extent)) {

        /* Free the empty extent at the beginning of the list */
        fs->fs_aextents = extent->ex_next;
        free(extent);
        return fs->fs_aextents;
    } else {
        return extent->ex_count ? extent : NULL;
    }
}

/* Free blocks in the specified extent if it was allocated for the layer */
static void
lc_freeExent(struct fs *fs, struct extent *fextent) {
    uint64_t block = fextent->ex_start, count = fextent->ex_count, freed;
    struct extent *extent;

    /* Check if the extent was allocated for the layer */
    while (count) {
        freed = 1;
        extent = fs->fs_aextents;
        while (extent) {
            if ((block >= extent->ex_start) &&
                (block < (extent->ex_start + extent->ex_count))) {
                freed = (extent->ex_start + extent->ex_count) - block;
                if (freed > count) {
                    freed = count;
                }
                lc_addExtent(fs->fs_gfs, &fs->fs_extents, block, freed);
                lc_updateAllocList(fs, extent, block, freed);
                break;
            }
            if ((block + freed) < extent->ex_start) {
                break;
            }
            extent = extent->ex_next;
        }
        block += freed;
        count -= freed;
    }
}

/* Add the extent to free list for reuse or deferred processing */
static void
lc_blockLayerFree(struct gfs *gfs, struct fs *fs, uint64_t block,
                  uint64_t count) {
    struct extent extent;

    if ((fs != lc_getGlobalFs(gfs)) && (fs->fs_aextents != NULL)) {
        extent.ex_start = block;
        extent.ex_count = count;
        lc_freeExent(fs, &extent);
    } else {
        lc_addExtent(fs->fs_gfs, &fs->fs_extents, block, count);
    }
}

/* Allocate specified number of blocks */
uint64_t
lc_blockAlloc(struct fs *fs, uint64_t count, bool meta, bool reserve) {
    struct gfs *gfs = fs->fs_gfs;
    uint64_t block;

    pthread_mutex_lock(&fs->fs_alock);
    if (meta) {
        if (fs->fs_blockMetaCount < count) {
            if (fs->fs_blockMetaCount) {
                lc_blockLayerFree(gfs, fs, fs->fs_blockMeta,
                                  fs->fs_blockMetaCount);
            }

            /* XXX Deal with a fragmented file system */
            fs->fs_blockMetaCount = (!reserve || count > LC_META_RESERVE) ?
                                count : LC_META_RESERVE;
            fs->fs_blockMeta = lc_findFreeBlock(gfs, fs,
                                                fs->fs_blockMetaCount,
                                                reserve);
            if ((fs->fs_blockMeta == LC_INVALID_BLOCK) &&
                (count < fs->fs_blockMetaCount)) {
                fs->fs_blockMetaCount = count;
                fs->fs_blockMeta = lc_findFreeBlock(gfs, fs,
                                                    fs->fs_blockMetaCount,
                                                    false);
                if (fs->fs_blockMeta == LC_INVALID_BLOCK) {
                    fs->fs_blockMetaCount = 0;
                    return LC_INVALID_BLOCK;
                }
            }
        }
        assert(fs->fs_blockMeta != LC_INVALID_BLOCK);
        assert(fs->fs_blockMetaCount >= count);
        block = fs->fs_blockMeta;
        fs->fs_blockMeta += count;
        fs->fs_blockMetaCount -= count;
    } else {
        block = lc_findFreeBlock(gfs, fs, count, true);
        if ((block == LC_INVALID_BLOCK) && (count == 1) &&
            fs->fs_blockMetaCount) {
            block = fs->fs_blockMeta;
            fs->fs_blockMeta += count;
            fs->fs_blockMetaCount -= count;
        }
    }
    pthread_mutex_unlock(&fs->fs_alock);
    assert((block + count) < gfs->gfs_super->sb_tblocks);
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
lc_blockFree(struct fs *fs, uint64_t block, uint64_t count) {
    struct gfs *gfs = getfs();

    assert(block && count);
    assert(block != LC_INVALID_BLOCK);
    assert((block + count) < gfs->gfs_super->sb_tblocks);
    if (fs) {

        /* Add blocks to the file system list for deferred processing */
        pthread_mutex_lock(&fs->fs_alock);
        lc_blockLayerFree(gfs, fs, block, count);
        pthread_mutex_unlock(&fs->fs_alock);
    } else {

        /* Add blocks back to the global free list */
        pthread_mutex_lock(&gfs->gfs_alock);
        lc_addExtent(gfs, &gfs->gfs_extents, block, count);
        assert(gfs->gfs_super->sb_blocks >= count);
        gfs->gfs_super->sb_blocks -= count;
        pthread_mutex_unlock(&gfs->gfs_alock);
    }
}

/* Free blocks allocated/reserved by a layer */
void
lc_freeLayerBlocks(struct gfs *gfs, struct fs *fs, bool unmount, bool remove) {
    struct extent *extent;

    /* Free unused blocks from the inode pool */
    if (fs->fs_blockInodesCount) {
        lc_blockFree(fs, fs->fs_blockInodes, fs->fs_blockInodesCount);
        fs->fs_blockInodesCount = 0;
        fs->fs_blockInodes = 0;
    }

    /* Free unused blocks from the metadata pool */
    if (fs->fs_blockMetaCount) {
        lc_blockFree(fs, fs->fs_blockMeta, fs->fs_blockMetaCount);
        fs->fs_blockMetaCount = 0;
        fs->fs_blockMeta = 0;
    }

    /* If the layer is being removed, then free any blocks allocated in the
     * layer, otherwise free the list
     */
    extent = fs->fs_aextents;
    if (unmount && extent) {
        fs->fs_aextents = NULL;
        assert(fs != lc_getGlobalFs(gfs));
        lc_blockFreeExtents(fs, extent, remove, !remove);

        /* Free blocks used for extents earlier */
        if (fs->fs_dextents) {
            lc_blockFreeExtents(lc_getGlobalFs(gfs), fs->fs_dextents,
                                true, false);
            fs->fs_dextents = NULL;
        }
    }

    /* Release any unused reserved blocks */
    fs->fs_freed += lc_blockFreeExtents(NULL, fs->fs_extents, true, false);
    fs->fs_extents = NULL;
}

/* Data extent for pending removal */
void
lc_freeLayerDataBlocks(struct fs *fs, uint64_t block, uint64_t count,
                       bool allocated) {

    /* XXX Do not free blocks right now as pages may be still in the process of
     * flushing.
     */
    if (0 && allocated) {
        lc_blockFree(fs, block, count);
    } else {
        //assert(fs != lc_getGlobalFs(fs->fs_gfs));
        pthread_mutex_lock(&fs->fs_alock);
        lc_addExtent(fs->fs_gfs, &fs->fs_fextents, block, count);
        pthread_mutex_unlock(&fs->fs_alock);
    }
}

void
lc_freeLayerMetaBlocks(struct fs *fs, uint64_t block, uint64_t count) {
    pthread_mutex_lock(&fs->fs_alock);
    lc_addExtent(fs->fs_gfs, &fs->fs_mextents, block, count);
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
        assert((extent->ex_start + extent->ex_count) <
               gfs->gfs_super->sb_tblocks);
        lc_freeLayerMetaBlocks(fs, extent->ex_start, extent->ex_count);
        if (insert) {
            extent->ex_start = block;
            extent->ex_count = count;
            tmp = extent->ex_next;
            extent->ex_next = NULL;
            insert = false;
            extent = tmp;
        } else {
            tmp = extent;
            extent = extent->ex_next;
            free(tmp);
        }
    }
    if (insert) {
        assert((*extents) == NULL);
        lc_addExtent(fs->fs_gfs, extents, block, count);
    }
}

/* Free metadata blocks allocated and freed in a layer */
void
lc_processFreedBlocks(struct fs *fs, bool remove) {
    if (fs->fs_fextents) {
        lc_blockFreeExtents(fs, fs->fs_fextents, remove, false);
        fs->fs_fextents = NULL;
    }
    if (fs->fs_mextents) {
        lc_blockFreeExtents(fs, fs->fs_mextents, remove, false);
        fs->fs_mextents = NULL;
    }
}

/* Update free space information to disk and free extent list */
void
lc_blockAllocatorDeinit(struct gfs *gfs, struct fs *fs) {
    uint64_t count = 0, pcount, block = LC_INVALID_BLOCK, bcount = 0;
    struct extent *extent;

    /* Free previously used blocks for storing free extent info */
    lc_blockFreeExtents(NULL, fs->fs_dextents, true, false);
    fs->fs_dextents = NULL;

    /* Count the number of free extents to find number of blocks needed */
    extent = gfs->gfs_extents;
    while (extent) {
        count++;
        bcount += extent->ex_count;
        assert((extent->ex_start + extent->ex_count) <
               gfs->gfs_super->sb_tblocks);
        extent = extent->ex_next;
    }
    pcount = (count + LC_EXTENT_BLOCK - 1) / LC_EXTENT_BLOCK;
    assert(pcount);

    /* Allocate blocks */
    extent = gfs->gfs_extents;
    while (extent) {
        if (extent->ex_count >= pcount) {
            block = extent->ex_start;
            extent->ex_start += pcount;
            extent->ex_count -= pcount;
            assert((extent->ex_start + extent->ex_count) <
                   gfs->gfs_super->sb_tblocks);
            break;
        }
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
    lc_blockFreeExtents(NULL, gfs->gfs_extents, false, true);
    gfs->gfs_extents = NULL;
}
