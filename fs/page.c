#include "includes.h"

static char dfs_zPage[DFS_BLOCK_SIZE];

#define DFS_PAGECACHE_SIZE  1
#define DFS_CLUSTER_SIZE    256

/* Return the hash number for the block number provided */
static inline int
dfs_pageBlockHash(uint64_t block) {
    return block % DFS_PCACHE_SIZE;
}

/* Allocate and initialize page block hash table */
struct pcache *
dfs_pcache_init() {
    struct pcache *pcache = malloc(sizeof(struct pcache) * DFS_PCACHE_SIZE);
    int i;

    for (i = 0; i < DFS_PCACHE_SIZE; i++) {
        pthread_mutex_init(&pcache[i].pc_lock, NULL);
        pcache[i].pc_head = NULL;
    }
    pcache->pc_pcount = 0;
    return pcache;
}

/* Insert a page into page hash */
static void
dfs_insertPage(struct fs *fs, struct page *page) {
    int hash = dfs_pageBlockHash(page->p_block);
    struct pcache *pcache = fs->fs_pcache;
    struct page *hpage;

    assert(page->p_data);
    hpage = malloc(sizeof(struct page));
    hpage->p_data = page->p_data;
    hpage->p_block = page->p_block;
    pthread_rwlock_init(&hpage->p_lock, NULL);
    assert(page->p_cnext == NULL);
    pthread_mutex_lock(&pcache[hash].pc_lock);
    hpage->p_cnext = pcache[hash].pc_head;
    pcache[hash].pc_head = hpage;
    pthread_mutex_unlock(&pcache[hash].pc_lock);
}

/* Lookup a page in the block hash */
static struct page *
dfs_lookupPage(struct fs *fs, uint64_t block) {
    int hash = dfs_pageBlockHash(block);
    struct pcache *pcache = fs->fs_pcache;
    struct page *page;
    bool incr;

    assert(block != DFS_PAGE_NOBLOCK);
    assert(block != DFS_PAGE_HOLE);
    pthread_mutex_lock(&pcache[hash].pc_lock);
    page = pcache[hash].pc_head;
    while (page) {
        if (page->p_block == block) {
            break;
        }
        page = page->p_cnext;
    }
    if (page == NULL) {
        page = malloc(sizeof(struct page));
        page->p_data = NULL;
        page->p_block = block;
        pthread_rwlock_init(&page->p_lock, NULL);
        page->p_cnext = pcache[hash].pc_head;
        pcache[hash].pc_head = page;
    }
    pthread_mutex_unlock(&pcache[hash].pc_lock);
    if (page && (page->p_data == NULL)) {
        incr = false;
        pthread_rwlock_wrlock(&page->p_lock);
        if (page->p_data == NULL) {
            page->p_data = dfs_readBlock(fs->fs_gfs->gfs_fd, block);
            incr = true;
        }
        pthread_rwlock_unlock(&page->p_lock);
        if (incr) {
            __sync_add_and_fetch(&pcache->pc_pcount, 1);
        }
    }
    return page;
}

/* Remove pages from page cache and free the hash table */
void
dfs_destroy_pages(struct pcache *pcache) {
    uint64_t count = 0;
    struct page *page;
    int i;

    for (i = 0; i < DFS_PCACHE_SIZE; i++) {
        while ((page = pcache[i].pc_head)) {
            pcache[i].pc_head = page->p_cnext;
            page->p_block = DFS_INVALID_BLOCK;
            page->p_cnext = NULL;
            pthread_rwlock_destroy(&page->p_lock);
            if (page->p_data) {
                free(page->p_data);
                count++;
            }
            free(page);
        }
        assert(pcache[i].pc_head == NULL);
        pthread_mutex_destroy(&pcache[i].pc_lock);
    }
    if (count) {
        __sync_sub_and_fetch(&pcache->pc_pcount, count);
    }
    assert(pcache->pc_pcount == 0);
    free(pcache);
}


/* Return the requested page if allocated already */
static struct page *
dfs_findDirtyPage(struct inode *inode, uint64_t pg) {
    return (pg < inode->i_pcount) ? &inode->i_page[pg] : NULL;
}

/* Add a bmap entry to the inode */
static void
dfs_inodeBmapAdd(struct inode *inode, uint64_t page, uint64_t block) {
    uint64_t lpage, count, size, tsize;
    uint64_t *blocks;

    assert(!inode->i_shared);
    lpage = (inode->i_stat.st_size + DFS_BLOCK_SIZE - 1) / DFS_BLOCK_SIZE;
    if (inode->i_bcount <= lpage) {
        assert(lpage >= page);
        count = inode->i_bcount ? (inode->i_bcount * 2) :
                                  (lpage ? (lpage + 1) : DFS_PAGECACHE_SIZE);
        while (count <= lpage) {
            count *= 2;
        }
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
    assert(page < inode->i_bcount);
    inode->i_bmap[page] = block;
}

/* Lookup inode bmap for the specified page */
static uint64_t
dfs_inodeBmapLookup(struct inode *inode, uint64_t page) {
    if ((page < inode->i_bcount) && inode->i_bmap[page]) {
        return inode->i_bmap[page];
    }
    return DFS_PAGE_HOLE;
}

/* Create a new page chain for the inode, copying existing page structures */
static void
dfs_copyBmap(struct inode *inode) {
    uint64_t *bmap = inode->i_bmap;
    uint64_t size = inode->i_bcount * sizeof(uint64_t);

    assert(inode->i_stat.st_blocks <= inode->i_bcount);
    inode->i_bmap = malloc(size);
    memcpy(inode->i_bmap, bmap, size);
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
        count = inode->i_pcount ? (inode->i_pcount * 2) :
                                  (lpage ? (lpage + 1) : DFS_PAGECACHE_SIZE);
        while (count <= lpage) {
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
            assert(inode->i_page == NULL);
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
            int *incr, struct fuse_bufvec *bufv) {
    struct page *page, *bpage;
    bool new = false;
    uint64_t block;
    char *data;

    assert(pg < inode->i_pcount);
    assert(!inode->i_shared);
    page = dfs_findDirtyPage(inode, pg);
    if (page->p_data == NULL) {
        if ((poffset != 0) || ((poffset + psize) != DFS_BLOCK_SIZE)) {
            block = dfs_inodeBmapLookup(inode, pg);
            if ((block != DFS_PAGE_HOLE) && (block != DFS_PAGE_NOBLOCK)) {
                bpage = dfs_lookupPage(inode->i_fs, block);
                data = bpage->p_data;
            } else {
                data = NULL;
            }
        } else {
            data = NULL;
        }
        posix_memalign((void **)&page->p_data, DFS_BLOCK_SIZE, DFS_BLOCK_SIZE);
        *incr = (*incr) + 1;
        if (poffset != 0) {
            if (data) {
                memcpy(page->p_data, data, poffset);
            } else {
                memset(page->p_data, 0, poffset);
            }
        }
        if ((poffset + psize) != DFS_BLOCK_SIZE) {
            if (data) {
                memcpy(&page->p_data[(poffset + psize)], data,
                    DFS_BLOCK_SIZE - (poffset + psize));
            } else {
                memset(&page->p_data[(poffset + psize)], 0,
                    DFS_BLOCK_SIZE - (poffset + psize));
            }
        }
    }
    dfs_updateVec(page, bufv, poffset, psize);
    return new;
}

/* Update pages of a file with provided data */
int
dfs_addPages(struct inode *inode, off_t off, size_t size,
             struct fuse_bufvec *bufv, struct fuse_bufvec *dst) {
    size_t wsize = size, psize;
    int count = 0, added = 0;
    uint64_t page, spage;
    off_t poffset;

    assert(S_ISREG(inode->i_stat.st_mode));

    /* Copy page headers if page chain is shared */
    if (inode->i_shared) {
        dfs_copyBmap(inode);
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
        count += dfs_addPage(inode, page, poffset, psize, &added, dst);
        page++;
        wsize -= psize;
    }
    wsize = fuse_buf_copy(dst, bufv, FUSE_BUF_SPLICE_NONBLOCK);
    assert(wsize == size);
    if (added) {
        __sync_add_and_fetch(&inode->i_fs->fs_pcount, added);
    }
    inode->i_blks += count;
    return count;
}

/* Read specified pages of a file */
void
dfs_readPages(struct inode *inode, off_t soffset, off_t endoffset,
              struct fuse_bufvec *bufv) {
    uint64_t block, pg = soffset / DFS_BLOCK_SIZE;
    off_t poffset, off = soffset, roff = 0;
    size_t psize, rsize = endoffset - off;
    struct fs *fs = inode->i_fs;
    struct page *page = NULL;
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

        page = dfs_findDirtyPage(inode, pg);
        if ((page == NULL) || (page->p_data == NULL)) {
            block = dfs_inodeBmapLookup(inode, pg);
            if (block == DFS_PAGE_HOLE) {
                bufv->buf[i].mem = dfs_zPage;
            } else {
                assert(block != DFS_PAGE_NOBLOCK);
                page = dfs_lookupPage(fs, block);
                bufv->buf[i].mem = &page->p_data[poffset];
            }
        } else {
            bufv->buf[i].mem = &page->p_data[poffset];
        }
        bufv->buf[i].size = psize;
        i++;
        pg++;
        roff += psize;
        rsize -= psize;
    }
    bufv->count = i;
}

/* Flush a cluster of pages */
static int
dfs_flushPageCluster(struct gfs *gfs, struct fs *fs, struct inode *inode,
                     uint64_t pg, struct page **pages, int count) {
    struct iovec *iovec;
    struct page *page;
    uint64_t block;
    int i;

    //dfs_printf("Flushing %d pages\n", count);
    /* XXX Do this for the whole file in a single step */
    block = dfs_blockAlloc(fs, count);
    if (count == 1) {
        page = pages[0];
        /* XXX Free old block */
        dfs_insertPage(fs, page);
        dfs_writeBlock(gfs->gfs_fd, page->p_data, block);
        dfs_inodeBmapAdd(inode, pg, block);
        page->p_data = NULL;
    } else {
        iovec = alloca(count * sizeof(struct iovec));
        for (i = 0; i < count; i++) {
            page = pages[i];
            /* XXX free the old block */
            dfs_inodeBmapAdd(inode, pg + i, block + i);
            dfs_insertPage(fs, page);
            iovec[i].iov_base = page->p_data;
            iovec[i].iov_len = DFS_BLOCK_SIZE;
            page->p_data = NULL;
        }
        dfs_writeBlocks(gfs->gfs_fd, iovec, count, block);
    }
    __sync_add_and_fetch(&fs->fs_pcache->pc_pcount, count);
    return count;
}

/* Flush dirty pages of an inode */
void
dfs_flushPages(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    int count = 0, max, freed = 0;
    struct page *page, **pages;
    uint64_t i, lpage, pcount;
    uint64_t pg;

    assert(S_ISREG(inode->i_stat.st_mode));
    //dfs_printf("Flushing pages of inode %ld\n", inode->i_stat.st_ino);
    if (inode->i_shared) {
        return;
    }
    lpage = (inode->i_stat.st_size + DFS_BLOCK_SIZE - 1) / DFS_BLOCK_SIZE;
    max = lpage;
    if (max == 0) {
        return;
    } else if (max > DFS_CLUSTER_SIZE) {
        max = DFS_CLUSTER_SIZE;
    }
    assert(lpage < inode->i_pcount);
    pages = alloca(max * sizeof(struct page *));
    for (i = 0; i <= lpage; i++) {
        page = &inode->i_page[i];
        if (page->p_data) {
            if ((count >= max) || (count && (i != (pg + 1))))  {
                freed += dfs_flushPageCluster(gfs, fs, inode, pg,
                                              pages, count);
                count = 0;
            }
            if (count == 0) {
                pg = i;
            }
            pages[count++] = page;
        }
    }
    if (count) {
        freed += dfs_flushPageCluster(gfs, fs, inode, pg, pages, count);
    }
    if (freed) {
        pcount = __sync_fetch_and_sub(&inode->i_fs->fs_pcount, freed);
        assert(pcount >= freed);
    }
    if (inode->i_page) {
        free(inode->i_page);
        inode->i_page = NULL;
        inode->i_pcount = 0;
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

    for (i = 0; i < inode->i_bcount; i++) {
        assert(inode->i_bmap[i] != DFS_PAGE_NOBLOCK);
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
    inode->i_stat.st_blocks = bcount;
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
    struct bmap *bmap;

    //dfs_printf("Reading bmap of inode %ld\n", inode->i_stat.st_ino);
    assert(S_ISREG(inode->i_stat.st_mode));
    if (inode->i_stat.st_size == 0) {
        assert(inode->i_stat.st_blocks == 0);
        return;
    }
    dfs_inodeAllocPages(inode);
    while (block != DFS_INVALID_BLOCK) {
        //dfs_printf("Reading bmap block %ld\n", block);
        bblock = dfs_readBlock(gfs->gfs_fd, block);
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

/* Truncate pages beyond the new size of the file */
uint64_t
dfs_truncPages(struct inode *inode, off_t size, bool remove) {
    uint64_t pg = size / DFS_BLOCK_SIZE, lpage;
    uint64_t i, bcount = 0, tcount = 0, pcount;
    struct page *page, *bpage;
    bool truncated = false;
    int freed = 0;
    off_t poffset;

    if ((inode->i_bmap == NULL) && (inode->i_pcount == 0)) {
        assert(inode->i_stat.st_blocks == 0);
        assert(inode->i_stat.st_size == 0);
        assert(inode->i_bcount == 0);
        assert(inode->i_pcount == 0);
        assert(inode->i_page == 0);
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
            inode->i_bcount = 0;
            inode->i_bmap = NULL;
            return 0;
        }
        dfs_copyBmap(inode);
    }
    assert(!inode->i_shared);
    lpage = (inode->i_stat.st_size + DFS_BLOCK_SIZE - 1) / DFS_BLOCK_SIZE;

    /* Remove blockmap entries past the new size */
    if (inode->i_bcount) {
        assert(inode->i_stat.st_blocks <= inode->i_bcount);
        for (i = pg; i < inode->i_bcount; i++) {
            if (inode->i_bmap[i] == 0) {
                continue;
            }
            if ((pg == i) && ((size % DFS_BLOCK_SIZE) != 0)) {

                /* If a page is partially truncated, keep it */
                poffset = size % DFS_BLOCK_SIZE;
                dfs_inodeAllocPages(inode);
                page = dfs_findDirtyPage(inode, pg);
                if (page->p_data == NULL) {
                    assert(inode->i_bmap[i] != DFS_PAGE_NOBLOCK);
                    posix_memalign((void **)&page->p_data,
                                   DFS_BLOCK_SIZE, DFS_BLOCK_SIZE);
                    __sync_add_and_fetch(&inode->i_fs->fs_pcount, 1);
                    bpage = dfs_lookupPage(inode->i_fs, inode->i_bmap[i]);
                    memcpy(page->p_data, bpage->p_data, poffset);
                }
                memset(&page->p_data[poffset], 0, DFS_BLOCK_SIZE - poffset);
                inode->i_bmap[i] = DFS_PAGE_NOBLOCK;
                inode->i_blks++;
                truncated = true;
            } else {
                inode->i_bmap[i] = 0;
                bcount++;
            }
        }
    }

    /* Remove pages past the new size from the dirty list */
    if (inode->i_pcount) {
        assert(lpage < inode->i_pcount);
        for (i = pg; i <= lpage; i++) {
            page = &inode->i_page[i];
            if (page->p_data == NULL) {
                continue;
            }
            if ((pg == i) && ((size % DFS_BLOCK_SIZE) != 0)) {

                /* If a page is partially truncated, keep it */
                if (!truncated) {
                    poffset = size % DFS_BLOCK_SIZE;
                    memset(&page->p_data[poffset], 0,
                           DFS_BLOCK_SIZE - poffset);
                }
            } else {
                free(page->p_data);
                page->p_data = NULL;
                freed++;
            }
        }
    }
    if (freed) {
        pcount = __sync_fetch_and_sub(&inode->i_fs->fs_pcount, freed);
        assert(pcount >= freed);
    }
    assert(inode->i_stat.st_blocks >= bcount);
    if (remove) {
        inode->i_stat.st_blocks -= bcount;
    }
    if (size == 0) {
        assert((inode->i_stat.st_blocks == 0) || !remove);
        if (inode->i_page) {
            free(inode->i_page);
            inode->i_page = NULL;
            inode->i_pcount = 0;
        }
        if (inode->i_bmap) {
            free(inode->i_bmap);
            inode->i_bmap = NULL;
            inode->i_bcount = 0;
        }
        assert(inode->i_pcount == 0);
        assert(inode->i_bcount == 0);
        inode->i_pcache = true;

        /* XXX Does not work after remount */
        tcount = inode->i_blks;
        inode->i_blks = 0;
    }
    return tcount;
}
