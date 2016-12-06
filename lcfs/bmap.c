#include "includes.h"

/* Allocate a bmap list for the inode */
void
lc_inodeBmapAlloc(struct inode *inode) {
    uint64_t lpage, count, size, tsize;
    struct fs *fs = inode->i_fs;
    uint64_t *blocks;

    assert(inode->i_dinode.di_size);
    assert(S_ISREG(inode->i_dinode.di_mode));
    lpage = (inode->i_dinode.di_size + LC_BLOCK_SIZE - 1) / LC_BLOCK_SIZE;
    if (inode->i_bcount <= lpage) {
        count = lpage + 1;
        tsize = count * sizeof(uint64_t);
        blocks = lc_malloc(fs, tsize, LC_MEMTYPE_BMAP);

        /* Copy existing list to the new list allocated */
        if (inode->i_bcount) {
            size = inode->i_bcount * sizeof(uint64_t);
            memcpy(blocks, inode->i_bmap, size);
            memset(&blocks[inode->i_bcount], 0, tsize - size);
            lc_free(fs, inode->i_bmap, size, LC_MEMTYPE_BMAP);
        } else {
            assert(inode->i_bmap == NULL);
            memset(blocks, 0, tsize);
        }
        inode->i_bcount = count;
        inode->i_bmap = blocks;
    }
}

/* Add a bmap entry to the inode */
void
lc_inodeBmapAdd(struct inode *inode, uint64_t page, uint64_t block) {
    assert(!inode->i_shared);
    assert(inode->i_extentLength == 0);
    assert(page < inode->i_bcount);
    if (inode->i_bmap[page] == 0) {
        inode->i_dinode.di_blocks++;
    }
    inode->i_bmap[page] = block;
}

/* Lookup inode bmap for the specified page */
uint64_t
lc_inodeBmapLookup(struct inode *inode, uint64_t page) {

    /* Check if the inode has a single direct extent */
    if (inode->i_extentLength && (page < inode->i_extentLength)) {
        return inode->i_extentBlock + page;
    }

    /* If the file fragmented, lookup in the bmap hash table */
    if ((page < inode->i_bcount) && inode->i_bmap[page]) {
        return inode->i_bmap[page];
    }
    return LC_PAGE_HOLE;
}

/* Expand a single extent to a bmap list */
void
lc_expandBmap(struct inode *inode) {
    uint64_t i;

    assert(S_ISREG(inode->i_dinode.di_mode));
    inode->i_bmap = lc_malloc(inode->i_fs,
                              inode->i_extentLength * sizeof(uint64_t),
                              LC_MEMTYPE_BMAP);
    for (i = 0; i < inode->i_extentLength; i++) {
        inode->i_bmap[i] = inode->i_extentBlock + i;
    }
    inode->i_bcount = inode->i_extentLength;
    inode->i_extentBlock = 0;
    inode->i_extentLength = 0;
    assert(inode->i_dinode.di_blocks == inode->i_bcount);
    inode->i_bmapdirty = true;
}

/* Create a new bmap hash table for the inode, copying existing bmap table */
void
lc_copyBmap(struct inode *inode) {
    uint64_t *bmap;
    uint64_t size;

    assert(S_ISREG(inode->i_dinode.di_mode));
    assert(inode->i_extentLength == 0);
    bmap = inode->i_bmap;
    size = inode->i_bcount * sizeof(uint64_t);
    assert(inode->i_dinode.di_blocks <= inode->i_bcount);
    inode->i_bmap= lc_malloc(inode->i_fs, size, LC_MEMTYPE_BMAP);
    memcpy(inode->i_bmap, bmap, size);
    inode->i_shared = false;
}


/* Allocate a bmap block and flush to disk */
static uint64_t
lc_flushBmapBlocks(struct gfs *gfs, struct fs *fs,
                   struct page *fpage, uint64_t pcount) {
    uint64_t count = pcount, block;
    struct page *page = fpage;
    struct bmapBlock *bblock;

    block = lc_blockAllocExact(fs, pcount, true, true);
    while (page) {
        count--;
        lc_addPageBlockHash(gfs, fs, page, block + count);
        bblock = (struct bmapBlock *)page->p_data;
        bblock->bb_next = (page == fpage) ?
                          LC_INVALID_BLOCK : block + count + 1;
        page = page->p_dnext;
    }
    assert(count == 0);
    lc_flushPageCluster(gfs, fs, fpage, pcount);
    return block;
}

/* Flush blockmap of an inode */
void
lc_bmapFlush(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    uint64_t i, bcount = 0, pcount = 0;
    uint64_t block = LC_INVALID_BLOCK;
    struct bmapBlock *bblock = NULL;
    int count = LC_BMAP_BLOCK;
    struct page *page = NULL;
    struct bmap *bmap;

    assert(S_ISREG(inode->i_dinode.di_mode));

    /* If the file removed, nothing to write */
    if (inode->i_removed) {
        assert(inode->i_bmap == NULL);
        assert(inode->i_page == NULL);
        assert(inode->i_dpcount == 0);
        inode->i_bmapdirty = false;
        return;
    }
    if (inode->i_shared) {
        lc_copyBmap(inode);
    }
    lc_flushPages(gfs, fs, inode, true);
    if (inode->i_bcount) {
        lc_printf("File %ld fragmented\n", inode->i_dinode.di_ino);
    } else {
        block = inode->i_extentBlock;
        bcount = inode->i_extentLength;
    }

    /* Add bmap blocks with bmap entries */
    for (i = 0; i < inode->i_bcount; i++) {
        if (inode->i_bmap[i] == 0) {
            continue;
        }
        if (count >= LC_BMAP_BLOCK) {
            if (bblock) {
                page = lc_getPageNoBlock(gfs, fs, (char *)bblock, page);
            }
            lc_mallocBlockAligned(fs, (void **)&bblock, true);
            pcount++;
            count = 0;
        }
        bcount++;
        bmap = &bblock->bb_bmap[count++];
        bmap->b_off = i;
        bmap->b_block = inode->i_bmap[i];
    }
    assert(inode->i_dinode.di_blocks == bcount);
    if (bblock) {
        if (count < LC_BMAP_BLOCK) {
            bblock->bb_bmap[count].b_block = 0;
        }
        page = lc_getPageNoBlock(gfs, fs, (char *)bblock, page);
    }
    if (pcount) {
        block = lc_flushBmapBlocks(gfs, fs, page, pcount);
        lc_replaceMetaBlocks(fs, &inode->i_bmapDirExtents, block, pcount);
    }
    inode->i_bmapDirBlock = block;
    inode->i_bmapdirty = false;
    inode->i_dirty = true;
}

/* Read bmap blocks of a file and initialize page list */
void
lc_bmapRead(struct gfs *gfs, struct fs *fs, struct inode *inode,
             void *buf) {
    struct bmapBlock *bblock = buf;
    uint64_t i, bcount = 0;
    struct bmap *bmap;
    uint64_t block;

    assert(S_ISREG(inode->i_dinode.di_mode));
    if (inode->i_dinode.di_size == 0) {
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
    lc_printf("Inode %ld with fragmented extents %ld\n", inode->i_dinode.di_ino, inode->i_dinode.di_blocks);
    lc_inodeBmapAlloc(inode);
    bcount = inode->i_dinode.di_blocks;
    inode->i_dinode.di_blocks = 0;
    block = inode->i_bmapDirBlock;
    while (block != LC_INVALID_BLOCK) {
        //lc_printf("Reading bmap block %ld\n", block);
        lc_addExtent(gfs, fs, &inode->i_bmapDirExtents, block, 1);
        lc_readBlock(gfs, fs, block, bblock);
        for (i = 0; i < LC_BMAP_BLOCK; i++) {
            bmap = &bblock->bb_bmap[i];
            if (bmap->b_block == 0) {
                break;
            }
            //lc_printf("page %ld at block %ld\n", bmap->b_off, bmap->b_block);
            lc_inodeBmapAdd(inode, bmap->b_off, bmap->b_block);
        }
        block = bblock->bb_next;
    }
    assert(inode->i_dinode.di_blocks == bcount);
}

/* Free blocks in the extent list */
void
lc_freeInodeDataBlocks(struct fs *fs, struct inode *inode,
                       struct extent **extents) {
    struct extent *extent = *extents, *tmp;

    while (extent) {
        lc_freeLayerDataBlocks(fs, extent->ex_start, extent->ex_count,
                               inode->i_private);
        tmp = extent;
        extent = extent->ex_next;
        lc_free(fs, tmp, sizeof(struct extent), LC_MEMTYPE_EXTENT);
    }
}

/* Truncate the bmap of a file */
void
lc_bmapTruncate(struct gfs *gfs, struct fs *fs, struct inode *inode,
                size_t size, uint64_t pg, bool remove, bool *truncated) {
    struct extent *extents = NULL;
    uint64_t i, bcount = 0;

    /* Take care of files with single extent */
    if (remove && inode->i_extentLength) {
        assert(inode->i_bcount == 0);

        /* If a page is partially truncated, expand bmap */
        if (size % LC_BLOCK_SIZE) {
            lc_expandBmap(inode);
        } else {
            if (inode->i_extentLength > pg) {
                bcount = inode->i_extentLength - pg;
                lc_addExtent(gfs, fs, &extents,
                             inode->i_extentBlock + pg, bcount);
                inode->i_extentLength = pg;
            }
            if (inode->i_extentLength == 0) {
                inode->i_extentBlock = 0;
            }
        }
    }

    /* Remove blockmap entries past the new size */
    if (remove && inode->i_bcount) {
        assert(inode->i_dinode.di_blocks <= inode->i_bcount);
        for (i = pg; i < inode->i_bcount; i++) {
            if (inode->i_bmap[i] == 0) {
                continue;
            }
            if ((pg == i) && ((size % LC_BLOCK_SIZE) != 0)) {

                /* If a page is partially truncated, keep it */
                lc_truncatePage(fs, inode, NULL, pg, size % LC_BLOCK_SIZE);
                *truncated = true;
            } else {
                lc_addExtent(gfs, fs, &extents, inode->i_bmap[i], 1);
                inode->i_bmap[i] = 0;
                bcount++;
            }
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
        if (inode->i_bmap) {
            lc_free(fs, inode->i_bmap, inode->i_bcount * sizeof(uint64_t),
                    LC_MEMTYPE_BMAP);
            inode->i_bmap = NULL;
            inode->i_bcount = 0;
        }
        assert(inode->i_bcount == 0);
        if (remove) {
            inode->i_private = true;
        }
    }
}
