#include "includes.h"

#ifdef DEBUG

/* Copy extents in a list to another list */
static void
lc_copyExtents(struct gfs *gfs, struct fs *fs, struct extent *src,
               struct extent **dst, struct fs *ffs) {
    struct extent *extent = src, *tmp;
    uint64_t start;

    while (extent) {
        start = (extent->ex_type == LC_EXTENT_EMAP) ?
                    lc_getExtentBlock(extent) : lc_getExtentStart(extent);
        lc_addSpaceExtent(gfs, fs, dst, start, lc_getExtentCount(extent),
                          true);
        tmp = extent;
        extent = extent->ex_next;
        if (ffs) {
            lc_free(ffs, tmp, sizeof(struct extent), LC_MEMTYPE_EXTENT);
        }
    }
}

/* Check if extent allocated in a layer */
static void
lc_checkExtent(struct gfs *gfs, struct fs *fs, struct fs *rfs,
               uint64_t block, uint64_t count, struct extent **extents) {
    struct extent *extent = fs->fs_aextents;
    uint64_t estart, ecount, found;

    assert(extent);
    while (count) {
        found = 1;
        while (extent) {
            estart = lc_getExtentStart(extent);
            if (block < estart) {
                break;
            }
            ecount = lc_getExtentCount(extent);
            if (block < (estart + ecount)) {
                found = (estart + ecount) - block;
                if (found > count) {
                    found = count;
                }
                lc_addSpaceExtent(gfs, rfs, extents, block, found, true);
                break;
            }
            extent = extent->ex_next;
        }
        block += found;
        count -= found;
    }
}

/* Find blocks allocated to a layer */
static void
lc_findAllocatedBlocks(struct gfs *gfs, struct fs *fs, struct fs *rfs,
                       struct extent **extents) {
    uint64_t count = 0, icount = fs->fs_icount;
    struct extent *extent;
    struct inode *inode;
    int i;

    for (i = 0; (i < fs->fs_icacheSize) && (count < icount); i++) {
        inode = fs->fs_icache[i].ic_head;
        while (inode) {
            count++;
            if (inode->i_private) {
                if (inode->i_extentLength) {
                    lc_addSpaceExtent(gfs, rfs, extents, inode->i_extentBlock,
                                      inode->i_extentLength, true);
                } else {
                    lc_copyExtents(gfs, rfs, lc_inodeGetEmap(inode), extents,
                                   NULL);
                }
            } else if (S_ISREG(inode->i_mode)) {
                if (inode->i_extentLength) {
                    lc_checkExtent(gfs, fs, rfs, inode->i_extentBlock,
                                   inode->i_extentLength, extents);
                } else {
                    extent = lc_inodeGetEmap(inode);
                    while (extent) {
                        lc_checkExtent(gfs, fs, rfs, lc_getExtentBlock(extent),
                                       lc_getExtentCount(extent), extents);
                        extent = extent->ex_next;
                    }
                }
            }

            /* Add emap or directory blocks */
            if (inode->i_emapDirExtents) {
                lc_copyExtents(gfs, rfs, inode->i_emapDirExtents, extents,
                               NULL);
            }

            /* Add extended attributes */
            if (inode->i_xattrData && inode->i_xattrExtents) {
                lc_copyExtents(gfs, rfs, inode->i_xattrExtents, extents, NULL);
            }
            inode = inode->i_cnext;
        }
    }
}

/* Validate allocated extents in a layer */
static void
lc_validateAllocatedBlocks(struct gfs *gfs, struct fs *fs, struct fs *rfs,
                           struct extent *extent, struct extent **extents) {
    struct extent *aextent = fs->fs_aextents, *tmp;

    while (extent) {
        if (aextent) {
            assert(lc_getExtentStart(extent) == lc_getExtentStart(aextent));
            assert(lc_getExtentCount(extent) == lc_getExtentCount(aextent));
            aextent = aextent->ex_next;
        }
        lc_addSpaceExtent(gfs, rfs, extents, lc_getExtentStart(extent),
                          lc_getExtentCount(extent), true);
        tmp = extent;
        extent = extent->ex_next;
        lc_free(rfs, tmp, sizeof(struct extent), LC_MEMTYPE_EXTENT);
    }
    assert(aextent == NULL);
}

/* Validate space allocated to inodes and free extent map are consistent */
void
lc_validate(struct gfs *gfs) {
    struct extent *extents = NULL, *lextents, *rextents = NULL;
    struct fs *fs, *rfs = lc_getGlobalFs(gfs);
    struct super *super;
    int i;

    assert(gfs->gfs_fextents == NULL);
    for (i = 0; i <= gfs->gfs_scount; i++) {
        fs = gfs->gfs_fs[i];
        if (fs) {
            assert(fs->fs_extents == NULL);
            assert(fs->fs_rextents == NULL);
            lextents = NULL;
            super = fs->fs_super;

            /* Space allocated for super block and extent maps belong to the
             * root layer.
             */
            if (i && (fs->fs_sblock != LC_INVALID_BLOCK)) {
                lc_addSpaceExtent(gfs, rfs, &rextents, fs->fs_sblock, 1, true);
            }
            if (super->sb_extentCount) {
                lc_addSpaceExtent(gfs, rfs, &rextents, super->sb_extentBlock,
                                  super->sb_extentCount, true);
            }

            /* Account space allocated for inode blocks */
            if (fs->fs_fextents) {
                assert(i == 0);
                lc_copyExtents(gfs, rfs, fs->fs_fextents, &lextents, NULL);
            }
            if (fs->fs_mextents) {
                assert(i);
                lc_copyExtents(gfs, rfs, fs->fs_mextents, &lextents, NULL);
            }
            if (fs->fs_iextents) {
                lc_copyExtents(gfs, rfs, fs->fs_iextents, &lextents, fs);
            }
            lc_findAllocatedBlocks(gfs, fs, rfs, &lextents);
            lc_validateAllocatedBlocks(gfs, fs, rfs, lextents, &extents);
        }
    }
    lc_copyExtents(gfs, rfs, rextents, &extents, rfs);

    /* Add all the free blocks and there should be a single extent covering the
     * whole file system.
     */
    lc_copyExtents(gfs, rfs, gfs->gfs_extents, &extents, NULL);
    assert(extents->ex_next == NULL);
    assert(lc_getExtentStart(extents) == LC_START_BLOCK);
    assert(lc_getExtentCount(extents) ==
           (gfs->gfs_super->sb_tblocks - LC_START_BLOCK));
    lc_free(rfs, extents, sizeof(struct extent), LC_MEMTYPE_EXTENT);
}

#endif
