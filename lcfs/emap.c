#include "includes.h"

/* Add an extent to an extent list tracking emap */
static void
lc_addEmapExtent(struct gfs *gfs, struct fs *fs, struct extent **extents,
                 uint64_t page, uint64_t block, uint64_t count) {
    uint64_t ecount;

    /* Break up the extent if it is too big to fit */
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
    struct extent *extent = extents ? *extents : lc_inodeGetEmap(inode);

    /* Continue searching from last extent if there is one, otherwise from the
     * beginning. Extent list is sorted, so stop when a later page is found.
     */
    while (extent &&
           (page >= (lc_getExtentStart(extent) + lc_getExtentCount(extent)))) {
        assert(extent->ex_type == LC_EXTENT_EMAP);
        lc_validateExtent(gfs, extent);
        extent = extent->ex_next;
    }

    /* Save the current extent for a future lookup */
    if (extents) {
        *extents = extent;
    }

    /* If the page is part of the extent, return the block */
    if (extent && (page >= lc_getExtentStart(extent)) &&
        (page < (lc_getExtentStart(extent) + lc_getExtentCount(extent)))) {
        return lc_getExtentBlock(extent) + (page - lc_getExtentStart(extent));
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

/* Remove specified extent from inode's emap and free blocks */
static void
lc_removeInodeExtents(struct gfs *gfs, struct fs *fs, struct inode *inode,
                      uint64_t page, uint64_t block, uint64_t pcount,
                      struct extent **extents) {
    uint64_t count = pcount, blk = block, pg = page, ecount;

    /* There could be multiple extents if the pcount is bigger than what a
     * single extent could store.
     */
    while (count) {
        ecount = lc_removeExtent(fs, lc_inodeGetEmapPtr(inode), pg, count);
        assert(ecount);
        assert(ecount <= count);

        /* Add the extent to a list for deferred freeing */
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
    struct extent *extent = lc_inodeGetEmap(inode);
    uint64_t end, ecount, bcount = 0;

    assert(!(inode->i_flags & LC_INODE_SHARED));
    assert(inode->i_extentLength == 0);
    assert(count);

    /* XXX Combine emap lookup and removal into a single operation */
    /* Remove existing blocks for the specified range from inode emap */
    while (count) {

        /* If there are no more extents, nothing more to do */
        if (extent == NULL) {
            if (bstart != LC_PAGE_HOLE) {
                inode->i_dinode.di_blocks += count;
            }
            break;
        }
        block = lc_inodeEmapExtentLookup(gfs, inode, page, &extent);
        if (block != LC_PAGE_HOLE) {

            /* Remove the block */
            if (bcount == 0) {
                pg = page;
                blk = block;
            }

            /* If this block is not contiguous to the previous one, free up the
             * ones we have accumulated so far.
             */
            if ((blk + bcount) != block) {
                lc_removeInodeExtents(gfs, fs, inode, pg, blk, bcount,
                                      extents);
                extent = lc_inodeGetEmap(inode);
                pg = page;
                blk = block;
                bcount = 1;
            } else {
                bcount++;
            }

            /* Decrement total block counting if punching a hole */
            if (bstart == LC_PAGE_HOLE) {
                inode->i_dinode.di_blocks--;
                assert(count == 1);
            }

            /* Check if the last extent has more blocks mapping to the
             * specified range of pages
             */
            if (extent && (count > 1) && (page >= lc_getExtentStart(extent)) &&
                (page < (lc_getExtentStart(extent) +
                         lc_getExtentCount(extent)))) {
                end = lc_getExtentStart(extent) + lc_getExtentCount(extent);
                if (end > page) {
                    ecount = end - page;
                    if (ecount > count) {
                        ecount = count;
                    }
                    if (ecount) {

                        /* Consume rest of the extent and continue */
                        bcount += (ecount - 1);
                        page += ecount;
                        count -= ecount;
                        continue;
                    }
                }
            }
        } else {
            if (bcount) {
                lc_removeInodeExtents(gfs, fs, inode, pg, blk, bcount,
                                      extents);
                extent = lc_inodeGetEmap(inode);
                bcount = 0;
            }

            /* Increment total block count when a new block allocated for a
             * page which did not have a block before.
             */
            if (bstart != LC_PAGE_HOLE) {
                inode->i_dinode.di_blocks++;
            }
        }
        page++;
        count--;
    }
    if (bcount) {
        lc_removeInodeExtents(gfs, fs, inode, pg, blk, bcount, extents);
    }

    /* Add newly allocated blocks unless punching a hole */
    if (bstart != LC_PAGE_HOLE) {
        lc_addEmapExtent(gfs, fs, lc_inodeGetEmapPtr(inode),
                         pstart, bstart, pcount);
    }
}

/* Expand a single direct extent to an emap list */
void
lc_expandEmap(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    assert(S_ISREG(inode->i_mode));
    assert(inode->i_dinode.di_blocks == inode->i_extentLength);
    lc_addEmapExtent(gfs, fs, lc_inodeGetEmapPtr(inode), 0,
                     inode->i_extentBlock, inode->i_extentLength);
    inode->i_extentBlock = 0;
    inode->i_extentLength = 0;
    lc_markInodeDirty(inode, LC_INODE_EMAPDIRTY);
}

/* Create a new emap extent list for the inode, copying emap list of parent */
void
lc_copyEmap(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    struct extent **extents = lc_inodeGetEmapPtr(inode), *extent = *extents;

    assert(S_ISREG(inode->i_mode));
    assert(inode->i_extentLength == 0);
    lc_inodeSetEmap(inode, NULL);
    while (extent) {
        assert(extent->ex_type == LC_EXTENT_EMAP);
        lc_validateExtent(gfs, extent);
        lc_addEmapExtent(gfs, fs, extents, lc_getExtentStart(extent),
                         lc_getExtentBlock(extent), lc_getExtentCount(extent));
        extents = &((*extents)->ex_next);
        extent = extent->ex_next;
    }
    inode->i_flags &= ~LC_INODE_SHARED;
}

/* Allocate a emap block and flush to disk */
static uint64_t
lc_flushEmapBlocks(struct gfs *gfs, struct fs *fs,
                   struct page *fpage, uint64_t pcount) {
    struct page *page = fpage, *tpage;
    uint64_t count = pcount, block;
    struct emapBlock *eblock;

    block = lc_blockAllocExact(fs, pcount, true, true);

    /* Link the blocks together */
    while (page) {
        count--;
        lc_addPageBlockHash(gfs, fs, page, block + count);
        eblock = (struct emapBlock *)page->p_data;
        eblock->eb_magic = LC_EMAP_MAGIC;
        eblock->eb_next = (page == fpage) ?
                          LC_INVALID_BLOCK : block + count + 1;
        lc_updateCRC(eblock, &eblock->eb_crc);
        tpage = page;
        page = page->p_dnext;
    }
    assert(count == 0);
    lc_addPageForWriteBack(gfs, fs, fpage, tpage, pcount);
    return block;
}

/* Flush blockmap of an inode */
void
lc_emapFlush(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    uint64_t bcount = 0, pcount = 0, block = LC_INVALID_BLOCK;
    struct emapBlock *eblock = NULL;
    int count = LC_EMAP_BLOCK;
    struct page *page = NULL;
    struct extent *extent;
    struct emap *emap;

    assert(S_ISREG(inode->i_mode));

    /* Flush all the dirty pages */
    lc_flushPages(gfs, fs, inode, true, false);
    extent = lc_inodeGetEmap(inode);
    if (extent) {
        lc_printf("File %ld fragmented\n", inode->i_ino);
    } else {
        bcount = inode->i_extentLength;
        if (bcount == 0) {
            block = LC_INVALID_BLOCK;
        } else {
            block = inode->i_extentBlock;
        }
    }

    /* Add emap blocks with emap entries */
    while (extent) {
        if (count >= LC_EMAP_BLOCK) {
            if (eblock) {
                page = lc_getPageNoBlock(gfs, fs, (char *)eblock, page);
            }
            lc_mallocBlockAligned(fs->fs_rfs, (void **)&eblock,
                                  LC_MEMTYPE_DATA);
            pcount++;
            count = 0;
        }
        emap = &eblock->eb_emap[count++];
        emap->e_off = lc_getExtentStart(extent);
        emap->e_block = lc_getExtentBlock(extent);
        emap->e_count = lc_getExtentCount(extent);
        bcount += emap->e_count;
        //lc_printf("page %ld at block %ld count %d\n", emap->e_off, emap->e_block, emap->e_count);
        extent = extent->ex_next;
    }
    assert(inode->i_dinode.di_blocks == bcount);
    if (eblock) {
        if (count < LC_EMAP_BLOCK) {
            eblock->eb_emap[count].e_block = 0;
        }
        page = lc_getPageNoBlock(gfs, fs, (char *)eblock, page);
    }
    if (pcount) {
        block = lc_flushEmapBlocks(gfs, fs, page, pcount);
        lc_replaceFreedExtents(fs, &inode->i_emapDirExtents, block, pcount);
    } else if (inode->i_emapDirExtents) {
        lc_addFreedExtents(fs, inode->i_emapDirExtents, false);
        inode->i_emapDirExtents = NULL;
    }

    /* Store the first emap block information in inode */
    inode->i_emapDirBlock = block;
    assert(inode->i_flags & LC_INODE_DIRTY);
    inode->i_flags &= ~LC_INODE_EMAPDIRTY;
}

/* Read emap blocks of a file and initialize emap list */
void
lc_emapRead(struct gfs *gfs, struct fs *fs, struct inode *inode,
             void *buf) {
    struct extent **extents = lc_inodeGetEmapPtr(inode);
    struct emapBlock *eblock = buf;
    uint64_t i, bcount = 0;
    struct emap *emap;
    uint64_t block;

    assert(S_ISREG(inode->i_mode));

    /* Nothing to read for an empty file */
    assert(inode->i_size || (inode->i_dinode.di_blocks == 0));
    if (inode->i_dinode.di_blocks == 0) {
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

    /* Read emap blocks */
    while (block != LC_INVALID_BLOCK) {
        lc_addSpaceExtent(gfs, fs, &inode->i_emapDirExtents, block, 1, false);
        lc_readBlock(gfs, fs, block, eblock);
        assert(eblock->eb_magic == LC_EMAP_MAGIC);
        lc_verifyBlock(eblock, &eblock->eb_crc);

        /* Process emap entries from the emap block */
        for (i = 0; i < LC_EMAP_BLOCK; i++) {
            emap = &eblock->eb_emap[i];
            if (emap->e_block == 0) {
                break;
            }
            assert(emap->e_count > 0);
            //lc_printf("page %ld at block %ld count %d\n", emap->e_off, emap->e_block, emap->e_count);
            lc_addEmapExtent(gfs, fs, extents,
                             emap->e_off, emap->e_block, emap->e_count);
            extents = &((*extents)->ex_next);
            inode->i_dinode.di_blocks += emap->e_count;
        }
        block = eblock->eb_next;
    }
    assert(inode->i_dinode.di_blocks == bcount);
}

/* Free blocks in the extent list */
void
lc_freeInodeDataBlocks(struct fs *fs, struct inode *inode,
                       struct extent **extents) {
    struct extent *extent = *extents, *tmp;

    while (extent) {
        lc_addFreedBlocks(fs, lc_getExtentStart(extent),
                          lc_getExtentCount(extent));
        tmp = extent;
        extent = extent->ex_next;
        lc_free(fs, tmp, sizeof(struct extent), LC_MEMTYPE_EXTENT);
    }
}

/* Truncate the emap of a file */
bool
lc_emapTruncate(struct gfs *gfs, struct fs *fs, struct inode *inode,
                size_t size, uint64_t pg, bool remove) {
    struct extent *extents = NULL, *extent, **prev, *next;
    uint64_t bcount = 0, estart, ecount, eblock, freed;
    bool zero = false;

    assert(remove || (size == 0));

    /* Take care of files with single extent */
    if (remove && inode->i_extentLength) {
        assert(lc_inodeGetEmap(inode) == NULL);

        /* If a page is partially truncated, expand emap */
        if (size % LC_BLOCK_SIZE) {
            lc_expandEmap(gfs, fs, inode);
        } else {
            if (inode->i_extentLength > pg) {

                /* Trim the extent */
                bcount = inode->i_extentLength - pg;
                lc_addSpaceExtent(gfs, fs, &extents,
                                  inode->i_extentBlock + pg, bcount, false);
                inode->i_extentLength = pg;
            }
            if (inode->i_extentLength == 0) {

                /* If the whole extent is freed, then reset start block */
                inode->i_extentBlock = 0;
            }
        }
    }

    /* Remove blockmap entries past the new size */
    if (lc_inodeGetEmap(inode)) {
        prev = lc_inodeGetEmapPtr(inode);
        extent = lc_inodeGetEmap(inode);
        while (extent) {
            assert(extent->ex_type == LC_EXTENT_EMAP);
            lc_validateExtent(gfs, extent);
            if (!remove) {

                /* Free the extent and continue on unmount */
                lc_freeExtent(gfs, fs, extent, lc_inodeGetEmapPtr(inode), true);
                extent = lc_inodeGetEmap(inode);
                continue;
            }
            estart = lc_getExtentStart(extent);
            ecount = lc_getExtentCount(extent);
            eblock = lc_getExtentBlock(extent);
            next = extent->ex_next;
            if (pg < estart) {

                /* Free blocks mapping to pages past the new size */
                bcount += ecount;
                lc_addSpaceExtent(gfs, fs, &extents, eblock, ecount, false);
                lc_freeExtent(gfs, fs, extent, prev, true);
            } else if (pg < (estart + ecount)) {
                freed = (estart + ecount) - pg;
                if ((size % LC_BLOCK_SIZE) != 0) {

                    /* If a page is partially truncated, keep it */
                    freed--;
                    zero = true;
                }
                if (freed) {

                    /* Trim the extent */
                    bcount += freed;
                    lc_addSpaceExtent(gfs, fs, &extents,
                                      eblock + ecount - freed, freed, false);
                    if (freed == ecount) {

                        /* Free the whole extent */
                        lc_freeExtent(gfs, fs, extent, prev, true);
                    } else {
                        lc_decrExtentCount(gfs, extent, freed);
                        prev = &extent->ex_next;
                    }
                } else {
                    prev = &extent->ex_next;
                }
            } else {

                /* Keep this extent */
                assert(bcount == 0);
                prev = &extent->ex_next;
            }
            extent = next;
        }
    }

    /* Free blocks */
    if (bcount) {
        lc_freeInodeDataBlocks(fs, inode, &extents);
        assert(inode->i_dinode.di_blocks >= bcount);
        inode->i_dinode.di_blocks -= bcount;
        lc_layerChanged(gfs, false, false);
    } else {
        assert(extents == NULL);
    }
    if (size == 0) {
        assert((inode->i_dinode.di_blocks == 0) || !remove);
        assert(lc_inodeGetEmap(inode) == NULL);
        if (remove) {

            /* This inode is not sharing any blocks with its parents */
            inode->i_private = 1;
        }
    }
    return zero;
}
