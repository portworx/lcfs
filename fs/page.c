#include "includes.h"

static char dfs_zPage[DFS_BLOCK_SIZE];
#define DFS_PAGECACHE_SIZE 128

/* Page structure used for caching a file system block */
struct page {

    /* Data associated with page of the file */
    char *p_data;

    /* Block mapping to */
    uint64_t p_block;

    /* Set if page is dirty and needs to be flushed */
    bool p_dirty;

    /* Page is shared with other inodes in the snapshot chain */
    bool p_shared;
} __attribute__((packed));

/* Return the requested page if allocated already */
static struct page *
dfs_findPage(struct inode *inode, uint64_t pg) {
    return (pg < inode->i_pcount) ? &inode->i_page[pg] : NULL;
}

/* Create a new page chain for the inode, copying existing page structures */
static void
dfs_copyPages(struct inode *inode) {
    struct page *page, *opage;
    uint64_t i, count = 0;

    opage = inode->i_page;
    inode->i_page = malloc(inode->i_pcount * sizeof(struct page));
    for (i = 0; i < inode->i_pcount; i++) {
        page = &inode->i_page[i];
        assert(!opage->p_dirty);
        page->p_data = opage[i].p_data;
        page->p_block = opage[i].p_block;
        page->p_shared = (page->p_data || page->p_block);
        if (page->p_shared) {
            count++;
        }
        page->p_dirty = false;
    }
    assert(inode->i_stat.st_blocks == count);
    inode->i_shared = false;
}

/* Add the page to bufvec */
static inline void
dfs_updateVec(struct page *page, struct fuse_bufvec *bufv,
               off_t poffset, size_t psize) {
    assert(page->p_data);
    bufv->buf[bufv->count].mem = &page->p_data[poffset];
    bufv->buf[bufv->count].size = psize;
    bufv->count++;
}

/* Allocate/extend inode page table */
void
dfs_inodeAllocPages(struct inode *inode) {
    uint64_t lpage, count, size, tsize;
    struct page *page;

    assert(!inode->i_shared);
    lpage = (inode->i_stat.st_size + DFS_BLOCK_SIZE - 1) / DFS_BLOCK_SIZE;
    if (inode->i_pcount <= lpage) {
        count = inode->i_pcount ?  (inode->i_pcount * 2) : DFS_PAGECACHE_SIZE;
        while (count < lpage) {
            count *= 2;
        }
        tsize = count * sizeof(struct page);
        page = malloc(tsize);
        if (inode->i_pcount) {
            size = inode->i_pcount * sizeof(struct page);
            memcpy(page, inode->i_page, size);
            memset(&page[inode->i_pcount], 0, tsize - size);
            free(inode->i_page);
        } else {
            memset(page, 0, tsize);
        }
        inode->i_pcount = count;
        inode->i_page = page;
    }
    assert(lpage <= inode->i_pcount);
}

/* Add or update existing page of the inode with new data provided */
static int
dfs_addPage(struct inode *inode, uint64_t pg, off_t poffset, size_t psize,
            struct fuse_bufvec *bufv) {
    struct page *page;
    bool read = false;
    bool newblock;
    char *data;

    assert(pg < inode->i_pcount);
    page = dfs_findPage(inode, pg);
    if (page->p_data || page->p_block) {

        /* Allocate a new page if current page is shared */
        newblock = page->p_shared;
        if (page->p_shared || (page->p_data == NULL)) {
            data = page->p_data;
            if ((data == NULL) &&
                ((poffset != 0) || ((poffset + psize) != DFS_BLOCK_SIZE))) {
                data = dfs_readBlock(inode->i_fs->fs_gfs->gfs_fd,
                                     page->p_block);
                read = true;
            }
            posix_memalign((void **)&page->p_data, DFS_BLOCK_SIZE,
                           DFS_BLOCK_SIZE);
            __sync_add_and_fetch(&inode->i_fs->fs_pcount, 1);
            if (poffset != 0) {
                memcpy(page->p_data, data, poffset);
            }
            if ((poffset + psize) != DFS_BLOCK_SIZE) {
                memcpy(&page->p_data[(poffset + psize)], data,
                       DFS_BLOCK_SIZE - (poffset + psize));
            }
            if (read) {
                free(data);
                read = false;
            }
            if (page->p_shared) {
                page->p_block = 0;
                page->p_shared = false;
            }
        }
        assert(!page->p_shared);
        dfs_updateVec(page, bufv, poffset, psize);
        page->p_dirty = true;
        return newblock ? 1 : 0;
    }

    /* Allocate a new page */
    assert(!page->p_shared);
    posix_memalign((void **)&page->p_data, DFS_BLOCK_SIZE, DFS_BLOCK_SIZE);
    __sync_add_and_fetch(&inode->i_fs->fs_pcount, 1);
    if (poffset != 0) {
        memset(page->p_data, 0, poffset);
    }
    if ((poffset + psize) != DFS_BLOCK_SIZE) {
        memset(&page->p_data[(poffset + psize)], 0,
               DFS_BLOCK_SIZE - (poffset + psize));
    }
    dfs_updateVec(page, bufv, poffset, psize);
    page->p_dirty = true;
    inode->i_stat.st_blocks++;
    return 1;
}

/* Update pages of a file with provided data */
int
dfs_addPages(struct inode *inode, off_t off, size_t size,
             struct fuse_bufvec *bufv, struct fuse_bufvec *dst) {
    size_t wsize = size, psize;
    uint64_t page, spage;
    off_t poffset;
    int count = 0;

    assert(S_ISREG(inode->i_stat.st_mode));

    /* Copy page headers if page chain is shared */
    if (inode->i_shared) {
        dfs_copyPages(inode);
    }
    dfs_inodeAllocPages(inode);
    spage = off / DFS_BLOCK_SIZE;
    page = spage;

    /* Break the down the write into pages and link those to the file */
    while (wsize) {
        if (page == spage) {
            poffset = off % DFS_BLOCK_SIZE;
            psize = DFS_BLOCK_SIZE - poffset;
        } else {
            poffset = 0;
            psize = DFS_BLOCK_SIZE;
        }
        if (psize > wsize) {
            psize = wsize;
        }
        count += dfs_addPage(inode, page, poffset, psize, dst);
        page++;
        wsize -= psize;
    }
    wsize = fuse_buf_copy(dst, bufv, FUSE_BUF_SPLICE_NONBLOCK);
    assert(wsize == size);
    return count;
}

/* Read specified pages of a file */
void
dfs_readPages(struct inode *inode, off_t soffset, off_t endoffset,
              struct fuse_bufvec *bufv) {
    uint64_t pg = soffset / DFS_BLOCK_SIZE;
    off_t poffset, off = soffset, roff = 0;
    size_t psize, rsize = endoffset - off;
    struct page *page = NULL;
    char *data;
    int i = 0;

    assert(S_ISREG(inode->i_stat.st_mode));
    while (rsize) {
        if (off == soffset) {
            poffset = soffset % DFS_BLOCK_SIZE;
            psize = DFS_BLOCK_SIZE - poffset;
        } else {
            poffset = 0;
            psize = DFS_BLOCK_SIZE;
        }
        if (psize > rsize) {
            psize = rsize;
        }

        page = dfs_findPage(inode, pg);
        if (page->p_data || page->p_block) {
            if (page->p_data == NULL) {
                data = dfs_readBlock(inode->i_fs->fs_gfs->gfs_fd,
                                     page->p_block);
                if (!page->p_shared) {
                    page->p_data = data;
                    __sync_add_and_fetch(&inode->i_fs->fs_pcount, 1);
                }
            } else {
                data = page->p_data;
            }
            bufv->buf[i].mem = &data[poffset];
        } else {
            bufv->buf[i].mem = dfs_zPage;
        }
        bufv->buf[i].size = psize;
        i++;
        pg++;
        roff += psize;
        rsize -= psize;
    }
    bufv->count = i;
}

/* Flush dirty pages of an inode */
void
dfs_flushPages(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    uint64_t i, block, lpage;
    struct page *page;

    assert(S_ISREG(inode->i_stat.st_mode));
    //dfs_printf("Flushing pages of inode %ld\n", inode->i_stat.st_ino);
    lpage = (inode->i_stat.st_size + DFS_BLOCK_SIZE - 1) / DFS_BLOCK_SIZE;
    assert(lpage < inode->i_pcount);
    for (i = 0; i <= lpage; i++) {
        page = &inode->i_page[i];
        if (page->p_dirty) {
            assert(!page->p_shared);
            block = page->p_block;
            if (block == 0) {
                block = dfs_blockAlloc(fs, 1);
                page->p_block = block;
            }
            //dfs_printf("Flushing page %ld to block %ld\n", i, block);
            dfs_writeBlock(gfs->gfs_fd, page->p_data, block);
            page->p_dirty = false;
        }
    }
}

/* Flush a bmap block */
static uint64_t
dfs_flushBmapBlock(struct gfs *gfs, struct fs *fs,
                   struct bmapBlock *bblock, int count) {
    uint64_t block = dfs_blockAlloc(fs, 1);

    if (count < DFS_BMAP_BLOCK) {
        memset(&bblock->bb_bmap[count], 0,
               (DFS_BMAP_BLOCK - count) * sizeof(struct bmap));
    }
    //dfs_printf("Flushing bmap block to block %ld\n", block);
    dfs_writeBlock(gfs->gfs_fd, bblock, block);
    return block;
}

/* Flush blockmap of the inode */
void
dfs_bmapFlush(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    uint64_t block = DFS_INVALID_BLOCK;
    struct bmapBlock *bblock = NULL;
    uint64_t i, lpage, bcount = 0;
    int count = DFS_BMAP_BLOCK;
    struct bmap *bmap;
    struct page *page;

    assert(S_ISREG(inode->i_stat.st_mode));
    dfs_flushPages(gfs, fs, inode);
    //dfs_printf("Flushing bmap of inode %ld\n", inode->i_stat.st_ino);
    lpage = (inode->i_stat.st_size + DFS_BLOCK_SIZE - 1) / DFS_BLOCK_SIZE;
    assert(lpage < inode->i_pcount);
    for (i = 0; i <= lpage; i++) {
        page = &inode->i_page[i];
        assert(!page->p_dirty);
        if (page->p_block == 0) {
            assert(page->p_data == NULL);
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
        bmap = &bblock->bb_bmap[count++];
        bmap->b_off = i;
        bmap->b_block = page->p_block;
        if (page->p_shared) {
            bmap->b_block |= DFS_BLOCK_SHARED;
        }
        bcount++;
    }
    assert(inode->i_stat.st_blocks == bcount);
    if (bblock) {
        block = dfs_flushBmapBlock(gfs, fs, bblock, count);
        free(bblock);
    }
    /* XXX Free old bmap blocks */
    inode->i_bmapDirBlock = block;
    inode->i_bmapdirty = false;
    inode->i_dirty = true;
}

/* Read bmap blocks of a file and initialize page list */
void
dfs_bmapRead(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    uint64_t block = inode->i_bmapDirBlock;
    struct bmapBlock *bblock = NULL;
    uint64_t i, bcount = 0;
    bool shared = false;
    struct bmap *bmap;
    struct page *page;

    //dfs_printf("Reading bmap of inode %ld\n", inode->i_stat.st_ino);
    assert(S_ISREG(inode->i_stat.st_mode));
    if (inode->i_stat.st_size > 0) {
        dfs_inodeAllocPages(inode);
    }
    while (block != DFS_INVALID_BLOCK) {
        //dfs_printf("Reading bmap block %ld\n", block);
        bblock = dfs_readBlock(gfs->gfs_fd, block);
        for (i = 0; i < DFS_BMAP_BLOCK; i++) {
            bmap = &bblock->bb_bmap[i];
            if (bmap->b_block == 0) {
                break;
            }
            //dfs_printf("page %ld at block %ld\n", bmap->b_off, bmap->b_block);
            page = dfs_findPage(inode, bmap->b_off);
            assert(page->p_data == NULL);
            page->p_block = bmap->b_block;
            if (page->p_block & DFS_BLOCK_SHARED) {
                page->p_block &= ~DFS_BLOCK_SHARED;
                page->p_shared = true;
                shared = true;
            }
            bcount++;
        }
        block = bblock->bb_next;
        free(bblock);
    }
    inode->i_pcache = !shared;
    assert(inode->i_stat.st_blocks == bcount);
}

/* Truncate pages beyond the new size of the file */
uint64_t
dfs_truncPages(struct inode *inode, off_t size, bool remove) {
    uint64_t pg = size / DFS_BLOCK_SIZE, lpage;
    uint64_t i, bcount = 0, tcount = 0;
    struct page *page;
    off_t poffset;
    char *data;
    bool read;

    if (inode->i_page == NULL) {
        assert(inode->i_stat.st_blocks == 0);
        assert(inode->i_stat.st_size == 0);
        assert(inode->i_pcount == 0);
        assert(!inode->i_shared);
        inode->i_pcache = true;
        return 0;
    }

    /* Copy page list before changing it */
    if (inode->i_shared) {
        if (size == 0) {
            inode->i_stat.st_blocks = 0;
            inode->i_shared = false;
            inode->i_pcache = true;
            inode->i_page = NULL;
            inode->i_pcount = 0;
            return 0;
        }
        dfs_copyPages(inode);
    }
    assert(!inode->i_shared);

    /* Remove pages past the new size */
    lpage = (inode->i_stat.st_size + DFS_BLOCK_SIZE - 1) / DFS_BLOCK_SIZE;
    assert(lpage < inode->i_pcount);
    for (i = pg; i <= lpage; i++) {
        page = &inode->i_page[i];
        if ((page->p_data == NULL) && (page->p_block == 0)) {
            assert(!page->p_shared);
            assert(!page->p_dirty);
            continue;
        }
        if ((pg == i) && ((size % DFS_BLOCK_SIZE) != 0)) {

            /* If a page is partially truncated, keep it */
            poffset = size % DFS_BLOCK_SIZE;
            if (page->p_shared) {
                data = page->p_data;
                read = (data == NULL);
                if (read) {
                    data = dfs_readBlock(inode->i_fs->fs_gfs->gfs_fd,
                                         page->p_block);
                }
                posix_memalign((void **)&page->p_data, DFS_BLOCK_SIZE,
                               DFS_BLOCK_SIZE);
                __sync_add_and_fetch(&inode->i_fs->fs_pcount, 1);
                memcpy(page->p_data, data, poffset);
                page->p_block = 0;
                page->p_shared = false;
                if (read) {
                    free(data);
                }
            }
            memset(&page->p_data[poffset], 0, DFS_BLOCK_SIZE - poffset);
            page->p_dirty = true;
        } else {
            if (page->p_shared) {
                page->p_shared = false;
            } else {
                if (page->p_data) {
                    __sync_sub_and_fetch(&inode->i_fs->fs_pcount, 1);
                    free(page->p_data);
                }
                tcount++;
            }
            page->p_block = 0;
            page->p_data = NULL;
            page->p_dirty = false;
            bcount++;
        }
    }
    assert(inode->i_stat.st_blocks >= bcount);
    if (remove) {
        inode->i_stat.st_blocks -= bcount;
    }
    if (size == 0) {
        assert((inode->i_stat.st_blocks == 0) || !remove);
        if (inode->i_page) {
            for (i = 0; i < inode->i_pcount; i++) {
                page = &inode->i_page[i];
                assert(page->p_data == NULL);
                assert(!page->p_dirty);
            }
            free(inode->i_page);
            inode->i_page = NULL;
            inode->i_pcount = 0;
        }
        assert(inode->i_pcount == 0);
        inode->i_pcache = true;
    }
    return tcount;
}
