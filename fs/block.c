#include "includes.h"

#define LC_META_RESERVE     1024
#define LC_BLOCK_RESERVE    8192

/* Initializes the block allocator */
void
lc_blockAllocatorInit(struct gfs *gfs) {
    struct extent *extent;

    extent = malloc(sizeof(struct extent));
    extent->ex_start = gfs->gfs_super->sb_nblock;
    extent->ex_count = gfs->gfs_super->sb_tblocks - gfs->gfs_super->sb_nblock;
    extent->ex_next = NULL;
    gfs->gfs_extents = extent;
    lc_printf("lc_blockAllocatorInit: super->sb_nblock %ld super->sb_blocks %ld\n", gfs->gfs_super->sb_nblock, gfs->gfs_super->sb_blocks);
}

/* Merge nearby extents */
static void
lc_mergeExtents(struct extent *extent, struct extent *prev) {
    struct extent *next = extent->ex_next;

    if (next && ((extent->ex_start + extent->ex_count) == next->ex_start)) {
        extent->ex_count += next->ex_count;
        extent->ex_next = next->ex_next;
        free(next);
    }
    if (prev && ((prev->ex_start + prev->ex_count) == extent->ex_start)) {
        prev->ex_count += extent->ex_count;
        prev->ex_next = extent->ex_next;
        free(extent);
    }
}

/* Add an extent to an extent list */
static void
lc_addExtent(struct extent **extents, uint64_t block, uint64_t count) {
    struct extent *extent = *extents, *prev = NULL, *new;

    /* Look if the freed blocks could be merged to an existing extent */
    while (extent) {
        if ((extent->ex_start + extent->ex_count) == block) {
            extent->ex_count += count;
            lc_mergeExtents(extent, NULL);
            return;
        }
        if ((block + count) == extent->ex_start) {
            extent->ex_start -= count;
            extent->ex_count += count;
            lc_mergeExtents(extent, prev);
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
                 uint64_t count) {
    uint64_t block = LC_INVALID_BLOCK, reserve = LC_BLOCK_RESERVE;
    struct extent **extents, *extent, *prev = NULL;

    /* Check if an extent with enough free blocks available */
    extents = fs ? &fs->fs_extents : &gfs->gfs_extents;
    extent = *extents;
    while (extent) {
        if (extent->ex_count >= count) {
            block = extent->ex_start;
            extent->ex_start += count;
            extent->ex_count -= count;

            /* Free the extent if it is fully consumed */
            if (extent->ex_count == 0) {
                if (prev == NULL) {
                    *extents = extent->ex_next;
                    free(extent);
                    extent = *extents;
                } else {
                    prev->ex_next = extent->ex_next;
                    free(extent);
                    extent = prev->ex_next;
                }
            }

            if (fs == NULL) {

                /* Update global usage */
                gfs->gfs_super->sb_blocks += count;
                if ((block + count) > gfs->gfs_super->sb_nblock) {
                    gfs->gfs_super->sb_nblock = block + count;
                }
            } else if (fs->fs_gindex) {

                /* Track allocated extents for a layer */
                lc_addExtent(&fs->fs_aextents, block, count);
            }
            assert(block < gfs->gfs_super->sb_nblock);
            return block;
        }
        prev = extent;
        extent = extent->ex_next;
    }

    /* If the layer does not have any reserved chunks, make one */
    if (fs) {
        if (count > reserve) {
            reserve = count;
        }
        pthread_mutex_lock(&gfs->gfs_alock);
        block = lc_findFreeBlock(gfs, NULL, reserve);
        pthread_mutex_unlock(&gfs->gfs_alock);
        if (block != LC_INVALID_BLOCK) {
            if (fs->fs_gindex) {
                lc_addExtent(&fs->fs_aextents, block, count);
            }

            /* Add unused blocks to the free reserve */
            if (count < reserve) {
                lc_addExtent(&fs->fs_extents, block + count,
                             reserve - count);
            }
        }
    }
    return block;
}

/* Allocate specified number of blocks */
uint64_t
lc_blockAlloc(struct fs *fs, uint64_t count, bool meta) {
    struct gfs *gfs = fs->fs_gfs;
    uint64_t block;

    pthread_mutex_lock(&fs->fs_alock);
    if (meta) {
        assert(count == 1);
        if (fs->fs_meta_count < count) {
            assert(fs->fs_meta_count == 0);
            fs->fs_meta_count = LC_META_RESERVE;
            fs->fs_meta_next = lc_findFreeBlock(gfs, fs, fs->fs_meta_count);
        }
        block = fs->fs_meta_next;
        fs->fs_meta_next += count;
        fs->fs_meta_count -= count;
    } else {
        block = lc_findFreeBlock(gfs, fs, count);
    }
    pthread_mutex_unlock(&fs->fs_alock);
    assert((block + count) < gfs->gfs_super->sb_tblocks);
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
        lc_addExtent(fs->fs_gindex ? &fs->fs_fextents : &fs->fs_extents,
                     block, count);
        pthread_mutex_unlock(&fs->fs_alock);
    } else {

        /* Add blocks back to the global free list */
        pthread_mutex_lock(&gfs->gfs_alock);
        lc_addExtent(&gfs->gfs_extents, block, count);
        assert(gfs->gfs_super->sb_blocks >= count);
        gfs->gfs_super->sb_blocks -= count;
        pthread_mutex_unlock(&gfs->gfs_alock);
    }
}

/* Free an extent list */
static void
lc_blockFreeExtents(struct extent *extents, bool efree) {
    struct extent *extent = extents, *tmp;

    while (extent) {
        tmp = extent;
        if (efree && extent->ex_count) {
            lc_blockFree(NULL, extent->ex_start, extent->ex_count);
        }
        extent = extent->ex_next;
        free(tmp);
    }
}

/* Update allocated extent list with the extent freed */
static struct extent *
lc_updateAllocList(struct fs *fs, struct extent *extent,
                   uint64_t block, uint64_t freed) {
    struct extent *new;

    assert(extent->ex_count >= freed);
    assert(block >= extent->ex_start);
    assert((block + freed) <= (extent->ex_start + extent->ex_count));
    assert(freed <= extent->ex_count);
    if (extent->ex_start == block) {
        extent->ex_start += freed;
        extent->ex_count -= freed;
    } else if ((block + freed) ==
               (extent->ex_start + extent->ex_count)) {
        extent->ex_count -= freed;
    } else {

        /* Split the extent */
        new = malloc(sizeof(struct extent));
        new->ex_start = block + freed;
        new->ex_count = (extent->ex_start + extent->ex_count) -
                        new->ex_start;
        assert(new->ex_count > 0);
        extent->ex_count = block - extent->ex_start;
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
static struct extent *
lc_freeExent(struct fs *fs, struct extent *fextent, struct extent *lextent,
             bool remove) {
    uint64_t block = fextent->ex_start, count = fextent->ex_count, freed;
    struct extent *extent;

    /* Check if the extent was allocated for the layer */
    while (count) {
        freed = 1;
        extent = lextent;
        if (extent && (block >= extent->ex_start) &&
            (block < (extent->ex_start + extent->ex_count))) {
            freed = (extent->ex_start + extent->ex_count) - block;
            if (freed > count) {
                freed = count;
            }
            lc_blockFree(NULL, block, freed);
            if (remove) {
                lc_updateAllocList(fs, extent, block, freed);
            }
        } else {
            extent = fs->fs_aextents;
            while (extent) {
                if ((block >= extent->ex_start) &&
                    (block < (extent->ex_start + extent->ex_count))) {
                    freed = (extent->ex_start + extent->ex_count) - block;
                    if (freed > count) {
                        freed = count;
                    }
                    lc_blockFree(NULL, block, freed);
                    if (remove) {
                        lextent = lc_updateAllocList(fs, extent, block, freed);
                    } else {
                        lextent = extent;
                    }
                    break;
                }
                if ((block + freed) < extent->ex_start) {
                    lextent = extent;
                    break;
                }
                extent = extent->ex_next;
            }
        }
        block += freed;
        count -= freed;
    }
    return lextent;
}

/* Free blocks allocated/reserved by a layer */
void
lc_freeLayerBlocks(struct gfs *gfs, struct fs *fs, bool remove) {
    struct extent *extent, *lextent = NULL;

    pthread_mutex_lock(&fs->fs_alock);

    /* Free unused blocks from the metadata pool */
    if (fs->fs_meta_count) {
        lc_blockFree(NULL, fs->fs_meta_next, fs->fs_meta_count);
        fs->fs_meta_count = 0;
        fs->fs_meta_next = 0;
    }

    /* Free the blocks freed in layer which were allocated in the layer */
    assert((fs->fs_fextents == NULL) || fs->fs_gindex);
    while ((extent = fs->fs_fextents)) {
        fs->fs_fextents = fs->fs_fextents->ex_next;
        lextent = lc_freeExent(fs, extent, lextent, remove);
        free(extent);
    }
    assert(fs->fs_fextents == NULL);

    /* If the layer is being removed, then free any blocks allocated in the
     * layer, otherwise free the list
     */
    assert((fs->fs_aextents == NULL) || fs->fs_gindex);
    lc_blockFreeExtents(fs->fs_aextents, remove);
    fs->fs_aextents = NULL;

    /* Release any unused reserved blocks */
    lc_blockFreeExtents(fs->fs_extents, true);
    fs->fs_extents = NULL;
    pthread_mutex_unlock(&fs->fs_alock);
}

/* Update free block information in superblock */
void
lc_updateBlockMap(struct gfs *gfs) {
    struct extent *extent = gfs->gfs_extents;

    /* Find the last free extent in the free list */
    while (extent) {
        if (extent->ex_next == NULL) {
            if ((extent->ex_start + extent->ex_count) ==
                gfs->gfs_super->sb_tblocks) {
                gfs->gfs_super->sb_nblock = extent->ex_start;
            }
            break;
        }
        extent = extent->ex_next;
    }
    lc_printf("lc_updateBlockMap: gfs->gfs_super->sb_nblock %ld gfs->gfs_super->sb_blocks %ld\n", gfs->gfs_super->sb_nblock, gfs->gfs_super->sb_blocks);
}

/* Free resources associated with block allocator */
void
lc_blockAllocatorDeinit(struct gfs *gfs) {
    lc_blockFreeExtents(gfs->gfs_extents, false);
    gfs->gfs_extents = NULL;
}

