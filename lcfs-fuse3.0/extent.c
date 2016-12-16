#include "includes.h"

/* Merge nearby extents */
static void
lc_mergeExtents(struct gfs *gfs, struct fs *fs,
                struct extent *extent, struct extent *next,
                struct extent *prev) {
    if (next &&
        lc_extentAdjacent(lc_getExtentStart(extent), lc_getExtentBlock(extent),
                          lc_getExtentCount(extent), lc_getExtentStart(next),
                          lc_getExtentBlock(next), lc_getExtentCount(next))) {
        lc_incrExtentCount(gfs, extent, lc_getExtentCount(next));
        extent->ex_next = next->ex_next;
        lc_free(fs, next, sizeof(struct extent), LC_MEMTYPE_EXTENT);
    }
    if (prev &&
        lc_extentAdjacent(lc_getExtentStart(prev), lc_getExtentBlock(prev),
                          lc_getExtentCount(prev), lc_getExtentStart(extent),
                          lc_getExtentBlock(extent),
                          lc_getExtentCount(extent))) {
        lc_incrExtentCount(gfs, prev, lc_getExtentCount(extent));
        prev->ex_next = extent->ex_next;
        lc_free(fs, extent, sizeof(struct extent), LC_MEMTYPE_EXTENT);
    }
}

/* Add an extent to an extent list */
void
lc_addExtent(struct gfs *gfs, struct fs *fs, struct extent **extents,
             uint64_t start, uint64_t block, uint64_t count) {
    struct extent *extent = *extents, *prev = NULL, *new;
    uint64_t estart, eblock, ecount;

    /* Look if the new extent could be merged to an existing extent */
    eblock = block ? block : start;
    assert((eblock + count) < gfs->gfs_super->sb_tblocks);
    while (extent) {
        estart = lc_getExtentStart(extent);
        eblock = lc_getExtentBlock(extent);
        ecount = lc_getExtentCount(extent);
        if (lc_extentAdjacent(estart, eblock, ecount, start, block, count)) {
            lc_incrExtentCount(gfs, extent, count);
            lc_mergeExtents(gfs, fs, extent, extent->ex_next, NULL);
            return;
        }
        if (lc_extentAdjacent(start, block, count, estart, eblock, ecount)) {
            lc_decrExtentStart(NULL, extent, count);
            lc_incrExtentCount(gfs, extent, count);
            lc_mergeExtents(gfs, fs, extent, NULL, prev);
            return;
        }
        if ((start < estart) || (block && ((start + count) == estart))) {
            break;
        } else if (block && ((estart + ecount) == start)) {
            prev = extent;
            extent = extent->ex_next;
            break;
        }
        assert(start > (estart + ecount));
        prev = extent;
        extent = extent->ex_next;
    }

    /* Need to add a new extent */
    new = lc_malloc(fs, sizeof(struct extent), LC_MEMTYPE_EXTENT);
    lc_initExtent(NULL, new, block ? LC_EXTENT_EMAP : LC_EXTENT_SPACE,
                  start, block, count, extent);
    if (prev == NULL) {
        *extents = new;
    } else {
        prev->ex_next = new;
    }
}

/* Remove an extent from the list and free it */
void
lc_freeExtent(struct gfs *gfs, struct fs *fs, struct extent *extent,
              struct extent *prev, struct extent **extents, bool layer) {
    if (prev == NULL) {
        *extents = extent->ex_next;
    } else {
        prev->ex_next = extent->ex_next;
    }
    lc_free(layer ? fs : lc_getGlobalFs(gfs), extent,
            sizeof(struct extent), LC_MEMTYPE_EXTENT);
}

/* Update extent after taking off the specified extent */
static void
lc_updateExtent(struct fs *fs, struct extent *extent, struct extent *prev,
                struct extent **extents, uint64_t estart, uint64_t ecount,
                uint64_t start, uint64_t freed) {
    struct gfs *gfs = fs->fs_gfs;
    struct extent *new;
    uint64_t block;
    bool release;

    assert(ecount >= freed);
    assert(start >= estart);
    assert((start + freed) <= (estart + ecount));
    if (estart == start) {
        lc_incrExtentStart(gfs, extent, freed);
        release = lc_decrExtentCount(gfs, extent, freed);
    } else if ((start + freed) == (estart + ecount)) {
        release = lc_decrExtentCount(gfs, extent, freed);
    } else {

        /* Split the extent */
        new = lc_malloc(fs, sizeof(struct extent), LC_MEMTYPE_EXTENT);
        block = lc_getExtentBlock(extent) + (start - estart) + freed;
        lc_initExtent(gfs, new, extent->ex_type, start + freed, block,
                      estart + ecount - (start + freed), extent->ex_next);
        release = lc_decrExtentCount(gfs, extent,
                                     freed + lc_getExtentCount(new));
        assert(!release);
        extent->ex_next = new;
    }

    if (release) {
        lc_freeExtent(gfs, fs, extent, prev, extents, true);
    }
}

/* Remove the specified extent if present from the extent list */
uint64_t
lc_removeExtent(struct fs *fs, struct extent **extents, uint64_t start,
                uint64_t count) {
    struct extent *extent = *extents, *prev = NULL;
    uint64_t estart, ecount, freed = 0;

    while (extent) {
        estart = lc_getExtentStart(extent);
        if (start < estart) {
            break;
        }
        if ((start >= estart) &&
            (start < (estart + lc_getExtentCount(extent)))) {
            ecount = lc_getExtentCount(extent);
            freed = (estart + ecount) - start;
            if (freed > count) {
                freed = count;
            }
            lc_updateExtent(fs, extent, prev, extents, estart, ecount,
                            start, freed);
            break;
        }
        prev = extent;
        extent = extent->ex_next;
    }
    return freed;
}

