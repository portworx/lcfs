#include "includes.h"

/* Allocate a bmap list for the inode */
void
lc_inodeBmapAlloc(struct inode *inode) {
    uint64_t lpage, count, size, tsize;
    uint64_t *blocks;

    assert(inode->i_stat.st_size);
    assert(S_ISREG(inode->i_stat.st_mode));
    lpage = (inode->i_stat.st_size + LC_BLOCK_SIZE - 1) / LC_BLOCK_SIZE;
    if (inode->i_bcount <= lpage) {
        count = lpage + 1;
        tsize = count * sizeof(uint64_t);
        blocks = malloc(tsize);

        /* Copy existing list to the new list allocated */
        if (inode->i_bcount) {
            size = inode->i_bcount * sizeof(uint64_t);
            memcpy(blocks, inode->i_bmap, size);
            memset(&blocks[inode->i_bcount], 0, tsize - size);
            free(inode->i_bmap);
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

    inode->i_bmap = malloc(inode->i_extentLength * sizeof(uint64_t));
    for (i = 0; i < inode->i_extentLength; i++) {
        inode->i_bmap[i] = inode->i_extentBlock + i;
    }
    inode->i_bcount = inode->i_extentLength;
    inode->i_extentBlock = 0;
    inode->i_extentLength = 0;
    assert(inode->i_stat.st_blocks == inode->i_bcount);
    inode->i_bmapdirty = true;
}

/* Create a new bmap hash table for the inode, copying existing bmap table */
void
lc_copyBmap(struct inode *inode) {
    uint64_t *bmap;
    uint64_t size;

    assert(inode->i_extentLength == 0);
    bmap = inode->i_bmap;
    size = inode->i_bcount * sizeof(uint64_t);
    assert(inode->i_stat.st_blocks <= inode->i_bcount);
    inode->i_bmap = malloc(size);
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

    block = lc_blockAlloc(fs, pcount, true);
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

    assert(S_ISREG(inode->i_stat.st_mode));

    /* If the file removed, nothing to write */
    if (inode->i_removed) {
        assert(inode->i_bmap == NULL);
        assert(inode->i_page == NULL);
        inode->i_bmapdirty = false;
        return;
    }
    if (inode->i_shared) {
        lc_copyBmap(inode);
    }
    lc_flushPages(gfs, fs, inode);
    if (inode->i_bcount) {
        lc_printf("File %ld fragmented\n", inode->i_stat.st_ino);
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
            malloc_aligned((void **)&bblock);
            pcount++;
            count = 0;
        }
        bcount++;
        bmap = &bblock->bb_bmap[count++];
        bmap->b_off = i;
        bmap->b_block = inode->i_bmap[i];
    }
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
    inode->i_stat.st_blocks = bcount;
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

    assert(S_ISREG(inode->i_stat.st_mode));
    if (inode->i_stat.st_size == 0) {
        assert(inode->i_stat.st_blocks == 0);
        assert(inode->i_extentLength == 0);
        return;
    }

    /* If inode has a single extent, nothing more to do */
    if (inode->i_extentLength) {
        assert(inode->i_stat.st_blocks == inode->i_extentLength);
        assert(inode->i_extentBlock != 0);
        return;
    }
    lc_printf("Inode %ld with fragmented extents %ld\n", inode->i_stat.st_ino, inode->i_stat.st_blocks);
    lc_inodeBmapAlloc(inode);
    block = inode->i_bmapDirBlock;
    while (block != LC_INVALID_BLOCK) {
        //lc_printf("Reading bmap block %ld\n", block);
        lc_addExtent(gfs, &inode->i_bmapDirExtents, block, 1);
        lc_readBlock(gfs, fs, block, bblock);
        for (i = 0; i < LC_BMAP_BLOCK; i++) {
            bmap = &bblock->bb_bmap[i];
            if (bmap->b_block == 0) {
                break;
            }
            //lc_printf("page %ld at block %ld\n", bmap->b_off, bmap->b_block);
            lc_inodeBmapAdd(inode, bmap->b_off, bmap->b_block);
            bcount++;
        }
        block = bblock->bb_next;
    }
    assert(inode->i_stat.st_blocks == bcount);
}

