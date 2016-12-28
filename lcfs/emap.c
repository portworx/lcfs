#include "includes.h"

/* Add an extent to an extent list tracking emap */
static void
lc_addEmapExtent(struct gfs *gfs, struct fs *fs, struct extent **extents,
                 uint64_t page, uint64_t block, uint64_t count) {
    uint64_t ecount;

    while (count) {
        ecount = count;
        if (ecount > LC_EXTENT_EMAP_MAX) {
            ecount = LC_EXTENT_EMAP_MAX;
        }
        lc_addExtent(gfs, fs, extents, page, block, ecount, true);
        page += ecount;
        block += ecount;
        count -= ecount;
    }
}

/* Check the inode extent list for the block mapping to the page */
static uint64_t
lc_inodeEmapExtentLookup(struct gfs *gfs, struct inode *inode, uint64_t page,
                         struct extent **extents) {
    struct extent *extent = extents ? *extents : inode->i_emap;

    while (extent) {
        assert(extent->ex_type == LC_EXTENT_EMAP);
        lc_validateExtent(gfs, extent);
        if (page < lc_getExtentStart(extent)) {
            if (extents) {
                *extents = extent;
                extents = NULL;
            }
            break;
        }
        if ((page >= lc_getExtentStart(extent)) &&
            (page < (lc_getExtentStart(extent) + lc_getExtentCount(extent)))) {
            if (extents) {
                *extents = extent;
            }
            return lc_getExtentBlock(extent) + (page -
                                                lc_getExtentStart(extent));
        }
        extent = extent->ex_next;
    }
    if (extents) {
        *extents = NULL;
    }
    return LC_PAGE_HOLE;
}

/* Lookup inode emap for the specified page */
uint64_t
lc_inodeEmapLookup(struct gfs *gfs, struct inode *inode, uint64_t page,
                   struct extent **extents) {

    /* Check if the inode has a single direct extent */
    if (inode->i_extentLength && (page < inode->i_extentLength)) {
        return inode->i_extentBlock + page;
    }

    /* If the file fragmented, lookup in the emap list */
    return lc_inodeEmapExtentLookup(gfs, inode, page, extents);
}

static void
lc_removeInodeExtents(struct gfs *gfs, struct fs *fs, struct inode *inode,
                      uint64_t page, uint64_t block, uint64_t pcount,
                      struct extent **extents) {
    uint64_t count = pcount, blk = block, pg = page, ecount;

    while (count) {
        ecount = lc_removeExtent(fs, &inode->i_emap, pg, count);
        assert(ecount <= count);
        lc_addSpaceExtent(gfs, fs, extents, blk, ecount, false);
        pg += ecount;
        blk += ecount;
        count -= ecount;
    }
}

/* Add newly allocated blocks to the emap of the inode */
void
lc_inodeEmapUpdate(struct gfs *gfs, struct fs *fs, struct inode *inode,
                   uint64_t pstart, uint64_t bstart, uint64_t pcount,
                   struct extent **extents) {
    uint64_t page = pstart, pg = -1, count = pcount, block, blk = -1;
    struct extent *extent = inode->i_emap;
    uint64_t end, ecount, bcount = 0;

    assert(!(inode->i_flags & LC_INODE_SHARED));
    assert(inode->i_extentLength == 0);
    assert(count);

    while (count) {
        if (extent == NULL) {
            inode->i_dinode.di_blocks += count;
            break;
        }
        block = lc_inodeEmapExtentLookup(gfs, inode, page, &extent);
        if (block != LC_PAGE_HOLE) {
            if (bcount == 0) {
                pg = page;
                blk = block;
            }
            if ((blk + bcount) != block) {
                lc_removeInodeExtents(gfs, fs, inode, pg, blk, bcount,
                                      extents);
                extent = inode->i_emap;
                pg = page;
                blk = block;
                bcount = 1;
            } else {
                bcount++;
            }

            /* Check if the last extent has more blocks */
            if (extent && (page >= lc_getExtentStart(extent)) &&
                (page < (lc_getExtentStart(extent) +
                         lc_getExtentCount(extent)))) {
                end = lc_getExtentStart(extent) + lc_getExtentCount(extent);
                if (end > page) {
                    ecount = end - page;
                    if (ecount > count) {
                        ecount = count;
                    }
                    if (ecount) {
                        bcount += (ecount - 1);
                        page += ecount;
                        count -= ecount;
                        continue;
                    }
                }
            }
        } else {
            inode->i_dinode.di_blocks++;
        }
        page++;
        count--;
    }
    if (bcount) {
        lc_removeInodeExtents(gfs, fs, inode, pg, blk, bcount, extents);
    }
    lc_addEmapExtent(gfs, fs, &inode->i_emap, pstart, bstart, pcount);
}

/* Expand a single extent to a emap list */
void
lc_expandEmap(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    assert(S_ISREG(inode->i_mode));
    assert(inode->i_dinode.di_blocks == inode->i_extentLength);
    lc_addEmapExtent(gfs, fs, &inode->i_emap, 0, inode->i_extentBlock,
                     inode->i_extentLength);
    inode->i_extentBlock = 0;
    inode->i_extentLength = 0;
    lc_markInodeDirty(inode, LC_INODE_EMAPDIRTY);
}

/* Create a new emap extent list for the inode, copying existing emap list */
void
lc_copyEmap(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    struct extent *extent = inode->i_emap;

    assert(S_ISREG(inode->i_mode));
    assert(inode->i_extentLength == 0);
    inode->i_emap = NULL;
    while (extent) {
        assert(extent->ex_type == LC_EXTENT_EMAP);
        lc_validateExtent(gfs, extent);
        lc_addEmapExtent(gfs, fs, &inode->i_emap, lc_getExtentStart(extent),
                         lc_getExtentBlock(extent), lc_getExtentCount(extent));
        extent = extent->ex_next;
    }
    inode->i_flags &= ~LC_INODE_SHARED;
}

/* Allocate a emap block and flush to disk */
static uint64_t
lc_flushEmapBlocks(struct gfs *gfs, struct fs *fs,
                   struct page *fpage, uint64_t pcount) {
    uint64_t count = pcount, block;
    struct page *page = fpage;
    struct emapBlock *bblock;

    block = lc_blockAllocExact(fs, pcount, true, true);
    while (page) {
        count--;
        lc_addPageBlockHash(gfs, fs, page, block + count);
        bblock = (struct emapBlock *)page->p_data;
        bblock->eb_next = (page == fpage) ?
                          LC_INVALID_BLOCK : block + count + 1;
        page = page->p_dnext;
    }
    assert(count == 0);
    lc_flushPageCluster(gfs, fs, fpage, pcount, false);
    return block;
}

/* Flush blockmap of an inode */
void
lc_emapFlush(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    uint64_t bcount = 0, pcount = 0, block = LC_INVALID_BLOCK;
    struct emapBlock *bblock = NULL;
    int count = LC_EMAP_BLOCK;
    struct page *page = NULL;
    struct extent *extent;
    struct emap *emap;

    assert(S_ISREG(inode->i_mode));

    /* If the file removed, nothing to write */
    if (inode->i_flags & LC_INODE_REMOVED) {
        assert(inode->i_emap == NULL);
        assert(inode->i_page == NULL);
        assert(inode->i_dpcount == 0);
        inode->i_flags &= ~LC_INODE_EMAPDIRTY;
        return;
    }
    lc_flushPages(gfs, fs, inode, true, false);
    extent = inode->i_emap;
    if (extent) {
        lc_printf("File %ld fragmented\n", inode->i_ino);
    } else {
        block = inode->i_extentBlock;
        bcount = inode->i_extentLength;
    }

    /* Add emap blocks with emap entries */
    while (extent) {
        if (count >= LC_EMAP_BLOCK) {
            if (bblock) {
                page = lc_getPageNoBlock(gfs, fs, (char *)bblock, page);
            }
            lc_mallocBlockAligned(fs->fs_rfs, (void **)&bblock,
                                  LC_MEMTYPE_DATA);
            pcount++;
            count = 0;
        }
        emap = &bblock->eb_emap[count++];
        emap->e_off = lc_getExtentStart(extent);
        emap->e_block = lc_getExtentBlock(extent);
        emap->e_count = lc_getExtentCount(extent);
        bcount += emap->e_count;
        //lc_printf("page %ld at block %ld count %d\n", emap->e_off, emap->e_block, emap->e_count);
        extent = extent->ex_next;
    }
    assert(inode->i_dinode.di_blocks == bcount);
    if (bblock) {
        if (count < LC_EMAP_BLOCK) {
            bblock->eb_emap[count].e_block = 0;
        }
        page = lc_getPageNoBlock(gfs, fs, (char *)bblock, page);
    }
    if (pcount) {
        block = lc_flushEmapBlocks(gfs, fs, page, pcount);
        lc_replaceMetaBlocks(fs, &inode->i_emapDirExtents, block, pcount);
    }
    inode->i_emapDirBlock = block;
    assert(inode->i_flags & LC_INODE_DIRTY);
    inode->i_flags &= ~LC_INODE_EMAPDIRTY;
}

/* Read emap blocks of a file and initialize emap list */
void
lc_emapRead(struct gfs *gfs, struct fs *fs, struct inode *inode,
             void *buf) {
    struct emapBlock *bblock = buf;
    uint64_t i, bcount = 0;
    struct emap *emap;
    uint64_t block;

    assert(S_ISREG(inode->i_mode));
    if (inode->i_size == 0) {
        assert(inode->i_dinode.di_blocks == 0);
        assert(inode->i_extentLength == 0);
        return;
    }

    /* If inode has a single extent, nothing more to do */
    if (inode->i_extentLength) {
        assert(inode->i_dinode.di_blocks == inode->i_extentLength);
        assert(inode->i_extentBlock != 0);
        return;
    }
    lc_printf("Inode %ld with fragmented extents, blocks %d\n", inode->i_ino, inode->i_dinode.di_blocks);
    bcount = inode->i_dinode.di_blocks;
    inode->i_dinode.di_blocks = 0;
    block = inode->i_emapDirBlock;
    while (block != LC_INVALID_BLOCK) {
        //lc_printf("Reading emap block %ld\n", block);
        lc_addSpaceExtent(gfs, fs, &inode->i_emapDirExtents, block, 1, false);
        lc_readBlock(gfs, fs, block, bblock);
        for (i = 0; i < LC_EMAP_BLOCK; i++) {
            emap = &bblock->eb_emap[i];
            if (emap->e_block == 0) {
                break;
            }
            assert(emap->e_count > 0);
            //lc_printf("page %ld at block %ld count %d\n", emap->e_off, emap->e_block, emap->e_count);
            lc_addEmapExtent(gfs, fs, &inode->i_emap,
                             emap->e_off, emap->e_block, emap->e_count);
            inode->i_dinode.di_blocks += emap->e_count;
        }
        block = bblock->eb_next;
    }
    assert(inode->i_dinode.di_blocks == bcount);
}

/* Free blocks in the extent list */
void
lc_freeInodeDataBlocks(struct fs *fs, struct inode *inode,
                       struct extent **extents) {
    struct extent *extent = *extents, *tmp;

    while (extent) {
        lc_freeLayerDataBlocks(fs, lc_getExtentStart(extent),
                               lc_getExtentCount(extent), inode->i_private);
        tmp = extent;
        extent = extent->ex_next;
        lc_free(fs, tmp, sizeof(struct extent), LC_MEMTYPE_EXTENT);
    }
}

/* Truncate the emap of a file */
void
lc_emapTruncate(struct gfs *gfs, struct fs *fs, struct inode *inode,
                size_t size, uint64_t pg, bool remove, bool *truncated) {
    struct extent *extents = NULL, *extent, *prev, *next;
    uint64_t bcount = 0, estart, ecount, eblock, freed;

    /* Take care of files with single extent */
    if (remove && inode->i_extentLength) {
        assert(inode->i_emap == NULL);

        /* If a page is partially truncated, expand emap */
        if (size % LC_BLOCK_SIZE) {
            lc_expandEmap(gfs, fs, inode);
        } else {
            if (inode->i_extentLength > pg) {
                bcount = inode->i_extentLength - pg;
                lc_addSpaceExtent(gfs, fs, &extents,
                                  inode->i_extentBlock + pg, bcount, false);
                inode->i_extentLength = pg;
            }
            if (inode->i_extentLength == 0) {
                inode->i_extentBlock = 0;
            }
        }
    }

    /* Remove blockmap entries past the new size */
    if (inode->i_emap) {
        prev = NULL;
        extent = inode->i_emap;
        while (extent) {
            assert(extent->ex_type == LC_EXTENT_EMAP);
            lc_validateExtent(gfs, extent);
            if (!remove) {
                lc_freeExtent(gfs, fs, extent, NULL, &inode->i_emap, true);
                extent = inode->i_emap;
                continue;
            }
            estart = lc_getExtentStart(extent);
            ecount = lc_getExtentCount(extent);
            eblock = lc_getExtentBlock(extent);
            next = extent->ex_next;
            if (pg < estart) {
                bcount += ecount;
                lc_addSpaceExtent(gfs, fs, &extents, eblock, ecount, false);
                lc_freeExtent(gfs, fs, extent, prev, &inode->i_emap, true);
            } else if (pg < (estart + ecount)) {
                freed = (estart + ecount) - pg;
                if ((size % LC_BLOCK_SIZE) != 0) {
                    freed--;

                    /* If a page is partially truncated, keep it */
                    lc_truncatePage(fs, inode, NULL, pg, size % LC_BLOCK_SIZE);
                    *truncated = true;
                }
                if (freed) {
                    bcount += freed;
                    lc_addSpaceExtent(gfs, fs, &extents,
                                      eblock + ecount - freed, freed, false);
                    if (freed == ecount) {
                        lc_freeExtent(gfs, fs, extent, prev, &inode->i_emap,
                                      true);
                    } else {
                        lc_decrExtentCount(gfs, extent, freed);
                        prev = extent;
                    }
                } else {
                    prev = extent;
                }
            } else {
                assert(bcount == 0);
                prev = extent;
            }
            extent = next;
        }
    }

    /* Free blocks */
    if (bcount) {
        lc_freeInodeDataBlocks(fs, inode, &extents);
        assert(inode->i_dinode.di_blocks >= bcount);
        inode->i_dinode.di_blocks -= bcount;
    } else {
        assert(extents == NULL);
    }
    if (size == 0) {
        assert((inode->i_dinode.di_blocks == 0) || !remove);
        assert(inode->i_emap == NULL);
        if (remove) {
            inode->i_private = true;
        }
    }
}
