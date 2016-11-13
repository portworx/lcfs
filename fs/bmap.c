#include "includes.h"

/* Allocate a bmap list for the inode */
void
dfs_inodeBmapAlloc(struct inode *inode) {
    uint64_t lpage, count, size, tsize;
    uint64_t *blocks;

    assert(inode->i_stat.st_size);
    lpage = (inode->i_stat.st_size + DFS_BLOCK_SIZE - 1) / DFS_BLOCK_SIZE;
    if (inode->i_bcount <= lpage) {
        count = lpage + 1;
        tsize = count * sizeof(uint64_t);
        blocks = malloc(tsize);
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
dfs_inodeBmapAdd(struct inode *inode, uint64_t page, uint64_t block) {
    assert(!inode->i_shared);
    assert(inode->i_extentLength == 0);
    assert(page < inode->i_bcount);
    inode->i_bmap[page] = block;
}

/* Lookup inode bmap for the specified page */
uint64_t
dfs_inodeBmapLookup(struct inode *inode, uint64_t page) {
    if (inode->i_extentLength && (page < inode->i_extentLength)) {
        return inode->i_extentBlock + page;
    }
    if ((page < inode->i_bcount) && inode->i_bmap[page]) {
        return inode->i_bmap[page];
    }
    return DFS_PAGE_HOLE;
}

/* Expand a single extent to a bmap list */
void
dfs_expandBmap(struct inode *inode) {
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

/* Create a new page chain for the inode, copying existing page structures */
void
dfs_copyBmap(struct inode *inode) {
    uint64_t *bmap;
    uint64_t size;

    if (inode->i_extentLength) {
        dfs_expandBmap(inode);
    } else {
        bmap = inode->i_bmap;
        size = inode->i_bcount * sizeof(uint64_t);
        assert(inode->i_stat.st_blocks <= inode->i_bcount);
        inode->i_bmap = malloc(size);
        memcpy(inode->i_bmap, bmap, size);
    }
    inode->i_shared = false;
}


/* Flush a bmap block */
static uint64_t
dfs_flushBmapBlock(struct gfs *gfs, struct fs *fs,
                   struct bmapBlock *bblock, int count) {
    uint64_t block = dfs_blockAlloc(fs, 1, true);

    if (count < DFS_BMAP_BLOCK) {
        memset(&bblock->bb_bmap[count], 0,
               (DFS_BMAP_BLOCK - count) * sizeof(struct bmap));
    }
    //dfs_printf("Flushing bmap block to block %ld\n", block);
    dfs_writeBlock(gfs, fs, bblock, block);
    return block;
}

/* Flush blockmap of the inode */
void
dfs_bmapFlush(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    uint64_t block = DFS_INVALID_BLOCK;
    struct bmapBlock *bblock = NULL;
    int count = DFS_BMAP_BLOCK;
    uint64_t i, bcount = 0;
    struct bmap *bmap;

    assert(S_ISREG(inode->i_stat.st_mode));
    if (inode->i_removed) {
        assert(inode->i_bmap == NULL);
        assert(inode->i_page == NULL);
        inode->i_bmapdirty = false;
        return;
    }
    dfs_flushPages(gfs, fs, inode);
    //dfs_printf("Flushing bmap of inode %ld\n", inode->i_stat.st_ino);

    if (inode->i_bcount) {
        dfs_printf("File %ld fragmented\n", inode->i_stat.st_ino);
    } else {
        block = inode->i_extentBlock;
        bcount = inode->i_extentLength;
    }
    for (i = 0; i < inode->i_bcount; i++) {
        if (inode->i_bmap[i] == 0) {
            continue;
        }
        if (count >= DFS_BMAP_BLOCK) {
            if (bblock) {
                block = dfs_flushBmapBlock(gfs, fs, bblock, count);
            } else {
                posix_memalign((void **)&bblock, DFS_BLOCK_SIZE,
                               DFS_BLOCK_SIZE);
            }
            bblock->bb_next = block;
            count = 0;
        }
        bcount++;
        bmap = &bblock->bb_bmap[count++];
        bmap->b_off = i;
        bmap->b_block = inode->i_bmap[i];
    }
    if (bblock) {
        block = dfs_flushBmapBlock(gfs, fs, bblock, count);
        free(bblock);
    }
    inode->i_stat.st_blocks = bcount;

    /* XXX Free old bmap blocks */
    inode->i_bmapDirBlock = block;
    inode->i_bmapdirty = false;
    inode->i_dirty = true;
}

/* Read bmap blocks of a file and initialize page list */
void
dfs_bmapRead(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    struct bmapBlock *bblock = NULL;
    uint64_t i, bcount = 0;
    struct bmap *bmap;
    uint64_t block;

    //dfs_printf("Reading bmap of inode %ld\n", inode->i_stat.st_ino);
    assert(S_ISREG(inode->i_stat.st_mode));
    if (inode->i_stat.st_size == 0) {
        assert(inode->i_stat.st_blocks == 0);
        return;
    }
    if (inode->i_extentLength) {
        assert(inode->i_stat.st_blocks == inode->i_extentLength);
        assert(inode->i_extentBlock != 0);
        return;
    }
    dfs_printf("Inode %ld with fragmented extents %ld\n", inode->i_stat.st_ino, inode->i_stat.st_blocks);
    dfs_inodeBmapAlloc(inode);
    block = inode->i_bmapDirBlock;
    while (block != DFS_INVALID_BLOCK) {
        //dfs_printf("Reading bmap block %ld\n", block);
        bblock = dfs_readBlock(gfs, fs, block);
        for (i = 0; i < DFS_BMAP_BLOCK; i++) {
            bmap = &bblock->bb_bmap[i];
            if (bmap->b_block == 0) {
                break;
            }
            //dfs_printf("page %ld at block %ld\n", bmap->b_off, bmap->b_block);
            dfs_inodeBmapAdd(inode, bmap->b_off, bmap->b_block);
            bcount++;
        }
        block = bblock->bb_next;
        free(bblock);
    }
    assert(inode->i_stat.st_blocks == bcount);
}

