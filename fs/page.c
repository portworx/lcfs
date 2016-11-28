#include "includes.h"

static char lc_zPage[LC_BLOCK_SIZE];

/* Return the hash number for the block number provided */
static inline int
lc_pageBlockHash(uint64_t block) {
    assert(block);
    assert(block != LC_INVALID_BLOCK);
    return block % LC_PCACHE_SIZE;
}

/* Allocate and initialize page block hash table */
struct pcache *
lc_pcache_init() {
    struct pcache *pcache = malloc(sizeof(struct pcache) * LC_PCACHE_SIZE);
    int i;

    for (i = 0; i < LC_PCACHE_SIZE; i++) {
        pthread_mutex_init(&pcache[i].pc_lock, NULL);
        pcache[i].pc_head = NULL;
        pcache[i].pc_pcount = 0;
    }
    return pcache;
}


/* Allocate a new page */
static struct page *
lc_newPage(struct gfs *gfs) {
    struct page *page = malloc(sizeof(struct page));

    page->p_data = NULL;
    page->p_block = LC_INVALID_BLOCK;
    page->p_refCount = 1;
    page->p_hitCount = 0;
    page->p_cnext = NULL;
    page->p_dnext = NULL;
    page->p_dvalid = 0;
    pthread_mutex_init(&page->p_dlock, NULL);
    __sync_add_and_fetch(&gfs->gfs_pcount, 1);
    //lc_printf("new page %p gfs->gfs_pcount %ld\n", page, gfs->gfs_pcount);
    return page;
}

/* Free a page */
static void
lc_freePage(struct gfs *gfs, struct page *page) {
    assert(page->p_refCount == 0);
    assert(page->p_block == LC_INVALID_BLOCK);
    assert(page->p_cnext == NULL);
    assert(page->p_dnext == NULL);

    if (page->p_data) {
        free(page->p_data);
    }
    pthread_mutex_destroy(&page->p_dlock);
    free(page);
    __sync_sub_and_fetch(&gfs->gfs_pcount, 1);
}

/* Release a page */
static void
lc_releasePage(struct gfs *gfs, struct fs *fs, struct page *page, bool read) {
    struct page *cpage, *prev = NULL, *fpage = NULL, *fprev = NULL;
    struct pcache *pcache = fs->fs_pcache;
    uint64_t hash, hit;

    hash = lc_pageBlockHash(page->p_block);
    pthread_mutex_lock(&pcache[hash].pc_lock);
    assert(page->p_refCount > 0);
    page->p_refCount--;
    if (read) {
        page->p_hitCount++;
    }

    /* Free a page if page cache is above limit */
    if (pcache[hash].pc_pcount > (LC_PAGE_MAX / LC_PCACHE_SIZE)) {
        cpage = pcache[hash].pc_head;
        hit = page->p_hitCount;

        /* Look for a page with lowest hit count */
        while (cpage) {
            if ((cpage->p_refCount == 0) && (cpage->p_hitCount <= hit)) {
                fprev = prev;
                fpage = cpage;
                hit = cpage->p_hitCount;
            }
            prev = cpage;
            cpage = cpage->p_cnext;
        }

        /* If a page is picked for freeing, take off page from block cache */
        if (fpage) {
            if (fprev == NULL) {
                pcache[hash].pc_head = fpage->p_cnext;
            } else {
                fprev->p_cnext = fpage->p_cnext;
            }
            fpage->p_block = LC_INVALID_BLOCK;
            fpage->p_cnext = NULL;
            pcache[hash].pc_pcount--;
        }
    }
    pthread_mutex_unlock(&pcache[hash].pc_lock);
    if (fpage) {
        __sync_add_and_fetch(&gfs->gfs_precycle, 1);
        lc_freePage(gfs, fpage);
    }
}

/* Release a linked list of pages */
void
lc_releasePages(struct gfs *gfs, struct fs *fs, struct page *head) {
    struct page *page = head, *next;

    while (page) {
        next = page->p_dnext;
        page->p_dnext = NULL;
        if (page->p_block == LC_INVALID_BLOCK) {
            assert(page->p_refCount == 1);
            page->p_refCount = 0;
            lc_freePage(gfs, page);
        } else {
            lc_releasePage(gfs, fs, page, false);
        }
        page = next;
    }
}

/* Release pages */
static void
lc_releaseReadPages(struct gfs *gfs, struct fs *fs,
                     struct page **pages, uint64_t pcount) {
    uint64_t i;

    for (i = 0; i < pcount; i++) {
        lc_releasePage(gfs, fs, pages[i], true);
    }
}

/* Add a page to page block hash list */
void
lc_addPageBlockHash(struct gfs *gfs, struct fs *fs,
                    struct page *page, uint64_t block) {
    struct pcache *pcache = fs->fs_pcache;
    int hash = lc_pageBlockHash(block);
    struct page *cpage;

    assert(page->p_block == LC_INVALID_BLOCK);
    page->p_block = block;
    pthread_mutex_lock(&pcache[hash].pc_lock);
    cpage = pcache[hash].pc_head;

    /* Invalidate previous instance of this block if there is one */
    while (cpage) {
        if (cpage->p_block == block) {
            assert(cpage->p_refCount == 0);
            cpage->p_block = LC_INVALID_BLOCK;
            break;
        }
        cpage = cpage->p_cnext;
    }
    page->p_cnext = pcache[hash].pc_head;
    pcache[hash].pc_head = page;
    pcache[hash].pc_pcount++;
    pthread_mutex_unlock(&pcache[hash].pc_lock);
}

/* Lookup a page in the block hash */
static struct page *
lc_getPage(struct fs *fs, uint64_t block, bool read) {
    int hash = lc_pageBlockHash(block);
    struct pcache *pcache = fs->fs_pcache;
    struct page *page, *new = NULL;
    struct gfs *gfs = fs->fs_gfs;
    bool hit = false;

    assert(block);
    assert(block != LC_PAGE_HOLE);

retry:
    pthread_mutex_lock(&pcache[hash].pc_lock);
    page = pcache[hash].pc_head;
    while (page) {

        /* If a page is found, increment reference count */
        if (page->p_block == block) {
            page->p_refCount++;
            hit = true;
            break;
        }
        page = page->p_cnext;
    }

    /* If page is not found, instantiate one */
    if ((page == NULL) && new) {
        page = new;
        new = NULL;
        page->p_block = block;
        page->p_cnext = pcache[hash].pc_head;
        pcache[hash].pc_head = page;
        pcache[hash].pc_pcount++;
    }
    pthread_mutex_unlock(&pcache[hash].pc_lock);

    /* If no page is found, allocate one and retry */
    if (page == NULL) {
        new = lc_newPage(gfs);
        assert(!new->p_dvalid);
        goto retry;
    }

    /* If raced with another thread, free the unused page */
    if (new) {
        new->p_refCount = 0;
        lc_freePage(gfs, new);
    }

    /* If page is missing data, read from disk */
    if (read && !page->p_dvalid) {
        pthread_mutex_lock(&page->p_dlock);
        if (!page->p_dvalid) {
            page->p_data = lc_readBlock(gfs, fs, block, page->p_data);
            page->p_dvalid = 1;
        }
        pthread_mutex_unlock(&page->p_dlock);
    }
    assert(page->p_refCount > 0);
    assert(!read || page->p_data);
    assert(!read || page->p_dvalid);
    assert(page->p_block == block);
    if (hit) {
        __sync_add_and_fetch(&gfs->gfs_phit, 1);
    } else if (read) {
        __sync_add_and_fetch(&gfs->gfs_pmissed, 1);
    }
    return page;
}

/* Link new data to the page of the file */
static struct page *
lc_getPageNew(struct gfs *gfs, struct fs *fs, uint64_t block, char *data) {
    struct page *page = lc_getPage(fs, block, false);

    assert(page->p_refCount == 1);
    if (page->p_data) {
        free(page->p_data);
    }
    page->p_data = data;
    page->p_dvalid = 1;
    page->p_hitCount = 0;
    return page;
}

/* Get a page with no block associated with */
struct page *
lc_getPageNoBlock(struct gfs *gfs, struct fs *fs, char *data,
                  struct page *prev) {
    struct page *page = lc_newPage(gfs);

    page->p_data = data;
    page->p_dvalid = 1;
    page->p_dnext = prev;
    return page;
}

/* Get a page for the block without reading it from disk, but making sure a
 * data buffer exists for copying in data.
 */
struct page *
lc_getPageNewData(struct fs *fs, uint64_t block) {
    struct page *page;

    page = lc_getPage(fs, block, false);
    if (page->p_data == NULL) {
        malloc_aligned((void **)&page->p_data);
    }
    page->p_hitCount = 0;
    return page;
}

/* Remove pages from page cache and free the hash table */
void
lc_destroy_pages(struct gfs *gfs, struct pcache *pcache, bool remove) {
    uint64_t i, count = 0, pcount;
    struct page *page;

    for (i = 0; i < LC_PCACHE_SIZE; i++) {
        pcount = 0;
        pthread_mutex_lock(&pcache[i].pc_lock);
        while ((page = pcache[i].pc_head)) {
            pcache[i].pc_head = page->p_cnext;
            page->p_block = LC_INVALID_BLOCK;
            page->p_cnext = NULL;
            page->p_dvalid = 0;
            lc_freePage(gfs, page);
            pcount++;
        }
        assert(pcount == pcache[i].pc_pcount);
        assert(pcache[i].pc_head == NULL);
        pthread_mutex_unlock(&pcache[i].pc_lock);
        pthread_mutex_destroy(&pcache[i].pc_lock);
        count += pcount;
    }
    if (count && remove) {
        __sync_add_and_fetch(&gfs->gfs_preused, count);
    }
    free(pcache);
}

/* Return the requested page if allocated already */
static inline struct dpage *
lc_findDirtyPage(struct inode *inode, uint64_t pg) {
    return (pg < inode->i_pcount) ? &inode->i_page[pg] : NULL;
}

/* Fill up a partial page */
static void
lc_fillPage(struct inode *inode, struct dpage *dpage, uint64_t pg) {
    uint16_t poffset, psize, dsize, eof;
    struct page *bpage;
    char *data, *pdata;
    uint64_t block;

    poffset = dpage->dp_poffset;
    psize = dpage->dp_psize;
    pdata = dpage->dp_data;

    /* If the page is written partially, check if a block exists */
    if ((poffset != 0) ||
        (((pg * LC_BLOCK_SIZE) + psize) < inode->i_stat.st_size)) {
        block = lc_inodeBmapLookup(inode, pg);
        if (block != LC_PAGE_HOLE) {
            bpage = lc_getPage(inode->i_fs, block, true);
            data = bpage->p_data;
        } else {
            data = NULL;
        }
    } else {
        data = NULL;
    }

    /* Copy partial data or zero out partial page if page is new */
    if (poffset != 0) {
        if (data) {
            memcpy(pdata, data, poffset);
        } else {
            memset(pdata, 0, poffset);
        }
        dpage->dp_poffset = 0;
    }
    if ((poffset + psize) != LC_BLOCK_SIZE) {
        dsize = LC_BLOCK_SIZE - (poffset + psize);
        if (data) {
            eof = (pg == (inode->i_stat.st_size / LC_BLOCK_SIZE)) ?
                  (inode->i_stat.st_size % LC_BLOCK_SIZE) : 0;
            if (eof) {
                assert(eof >= (poffset + psize));
                dsize = eof - (poffset + psize);
                memset(&pdata[eof], 0, LC_BLOCK_SIZE - eof);
            }
            if (dsize) {
                memcpy(&pdata[(poffset + psize)], &data[(poffset + psize)],
                       dsize);
            }
        } else {
            memset(&pdata[(poffset + psize)], 0, dsize);
        }
        dpage->dp_psize = LC_BLOCK_SIZE;
    }
    if (data) {
        lc_releasePage(inode->i_fs->fs_gfs, inode->i_fs, bpage, true);
    }
}

/* Remove a dirty page */
static inline char *
lc_removeDirtyPage(struct inode *inode, uint64_t pg, bool release) {
    struct dpage *page;
    char *pdata;

    assert(pg < inode->i_pcount);
    page = &inode->i_page[pg];
    pdata = page->dp_data;
    if (pdata) {
        if (release) {
            free(pdata);
        } else if ((page->dp_poffset != 0) ||
                   (page->dp_psize != LC_BLOCK_SIZE)) {
            lc_fillPage(inode, page, pg);
        }
        page->dp_data = NULL;
    }
    return release ? NULL : pdata;
}

/* Add the page to bufvec */
static inline void
lc_updateVec(char *pdata, struct fuse_bufvec *bufv,
              off_t poffset, size_t psize) {
    bufv->buf[bufv->count].mem = &pdata[poffset];
    bufv->buf[bufv->count].size = psize;
    bufv->count++;
}

/* Allocate/extend inode page table */
static void
lc_inodeAllocPages(struct inode *inode) {
    uint64_t lpage, count, size, tsize;
    struct dpage *page;

    assert(!inode->i_shared);
    lpage = (inode->i_stat.st_size + LC_BLOCK_SIZE - 1) / LC_BLOCK_SIZE;
    if (inode->i_pcount <= lpage) {
        count = inode->i_pcount ? (inode->i_pcount * 2) :
                                  (lpage ? (lpage + 1) : LC_PAGECACHE_SIZE);
        while (count <= lpage) {
            count *= 2;
        }
        tsize = count * sizeof(struct dpage);
        page = malloc(tsize);
        if (inode->i_pcount) {
            size = inode->i_pcount * sizeof(struct dpage);
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

/* Get a dirty page and filled up with valid data */
static char *
lc_getDirtyPage(struct inode *inode, uint64_t pg) {
    struct dpage *dpage;
    char *pdata;

    dpage = lc_findDirtyPage(inode, pg);
    pdata = dpage ? dpage->dp_data : NULL;
    if (pdata &&
        ((dpage->dp_poffset != 0) || (dpage->dp_psize != LC_BLOCK_SIZE))) {
        lc_fillPage(inode, dpage, pg);
    }
    return pdata;
}

/* Add or update existing page of the inode with new data provided */
static uint64_t
lc_mergePage(struct inode *inode, uint64_t pg, char *data,
             uint16_t poffset, uint16_t psize) {
    uint16_t doff, dsize;
    struct dpage *dpage;
    bool fill;

    assert(poffset >= 0);
    assert(poffset < LC_BLOCK_SIZE);
    assert(psize > 0);
    assert(psize <= LC_BLOCK_SIZE);
    assert(!inode->i_shared);
    assert(pg < inode->i_pcount);

    dpage = lc_findDirtyPage(inode, pg);

    /* If no dirty page exists, add the new page and return */
    if (dpage->dp_data == NULL) {
        dpage->dp_data = data;
        dpage->dp_poffset = poffset;
        dpage->dp_psize = psize;
        return 1;
    }

    /* If the current dirty page is partial page and this new write is not a
     * contiguos write, initialize exisitng page correctly.
     */
    if (((dpage->dp_poffset != 0) || (dpage->dp_psize != LC_BLOCK_SIZE)) &&
        ((poffset != dpage->dp_poffset) ||
         ((poffset + psize) != (dpage->dp_poffset + dpage->dp_psize)))) {
        fill = false;
        dsize = 0;
        if (poffset < dpage->dp_poffset) {
            if ((poffset + psize) < dpage->dp_poffset) {
                fill = true;
                doff = 0;
            } else {
                doff = poffset;
                dsize += (dpage->dp_poffset - poffset);
            }
        } else {
            doff = dpage->dp_poffset;
        }
        if (!fill &&
            ((poffset + psize) > (dpage->dp_poffset + dpage->dp_psize))) {
            if (poffset > (dpage->dp_poffset + dpage->dp_psize)) {
                fill = true;
            } else {
                dsize += (poffset + psize) -
                         (dpage->dp_poffset + dpage->dp_psize);
            }
        }
        if (fill) {
            lc_fillPage(inode, dpage, pg);
        } else {
            dpage->dp_poffset = doff;
            dpage->dp_psize += dsize;
        }
    }
    memcpy(&dpage->dp_data[poffset], &data[poffset], psize);
    free(data);
    return 0;
}

/* Copy in the provided pages */
uint64_t
lc_copyPages(off_t off, size_t size, struct dpage *dpages,
             struct fuse_bufvec *bufv, struct fuse_bufvec *dst) {
    uint64_t page, spage, pcount = 0, poffset;
    size_t wsize = size, psize;
    char *pdata;

    spage = off / LC_BLOCK_SIZE;
    page = spage;

    /* Break the down the write into pages */
    while (wsize) {
        if (page == spage) {
            poffset = off % LC_BLOCK_SIZE;
            psize = LC_BLOCK_SIZE - poffset;
        } else {
            poffset = 0;
            psize = LC_BLOCK_SIZE;
        }
        if (psize > wsize) {
            psize = wsize;
        }
        malloc_aligned((void **)&pdata);
        lc_updateVec(pdata, dst, poffset, psize);
        dpages[pcount].dp_data = pdata;
        dpages[pcount].dp_poffset = poffset;
        dpages[pcount].dp_psize = psize;
        pcount++;
        page++;
        wsize -= psize;
    }
    wsize = fuse_buf_copy(dst, bufv, FUSE_BUF_SPLICE_NONBLOCK);
    assert(wsize == size);
    return pcount;
}

/* Update pages of a file with provided data */
uint64_t
lc_addPages(struct inode *inode, off_t off, size_t size,
            struct dpage *dpages, uint64_t pcount) {
    uint64_t page, spage, count = 0;
    off_t endoffset = off + size;
    struct dpage *dpage;
    uint64_t added = 0;

    assert(S_ISREG(inode->i_stat.st_mode));

    spage = off / LC_BLOCK_SIZE;
    page = spage;

    /* Update inode size if needed */
    if (endoffset > inode->i_stat.st_size) {
        inode->i_stat.st_size = endoffset;
    }

    /* Copy page headers if page chain is shared */
    if (inode->i_shared) {
        lc_copyBmap(inode);
    }
    if (inode->i_extentLength) {
        lc_expandBmap(inode);
    }
    lc_inodeAllocPages(inode);

    /* Link the dirty pages to the inode, merging with any existing ones */
    while (count < pcount) {
        dpage = &dpages[count];
        added += lc_mergePage(inode, page, dpage->dp_data, dpage->dp_poffset,
                              dpage->dp_psize);
        page++;
        count++;
    }
    return added;
}

/* Read specified pages of a file */
void
lc_readPages(fuse_req_t req, struct inode *inode, off_t soffset,
             off_t endoffset, struct page **pages, struct fuse_bufvec *bufv) {
    uint64_t block, pg = soffset / LC_BLOCK_SIZE, pcount = 0, i = 0;
    off_t poffset, off = soffset, roff = 0;
    size_t psize, rsize = endoffset - off;
    struct fs *fs = inode->i_fs;
    struct page *page = NULL;
    char *data;

    assert(S_ISREG(inode->i_stat.st_mode));
    while (rsize) {
        if (off == soffset) {
            poffset = soffset % LC_BLOCK_SIZE;
            psize = LC_BLOCK_SIZE - poffset;
        } else {
            poffset = 0;
            psize = LC_BLOCK_SIZE;
        }
        if (psize > rsize) {
            psize = rsize;
        }

        /* Check if a dirty page exists */
        data = lc_getDirtyPage(inode, pg);
        if (data == NULL) {

            /* Check bmap to find the block of the page */
            block = lc_inodeBmapLookup(inode, pg);
            if (block == LC_PAGE_HOLE) {
                bufv->buf[i].mem = lc_zPage;
            } else {

                /* Get the page */
                page = lc_getPage(fs, block, true);
                bufv->buf[i].mem = &page->p_data[poffset];
                pages[pcount++] = page;
            }
        } else {
            bufv->buf[i].mem = &data[poffset];
        }
        bufv->buf[i].size = psize;
        i++;
        pg++;
        roff += psize;
        rsize -= psize;
    }
    bufv->count = i;
    fuse_reply_data(req, bufv, FUSE_BUF_SPLICE_MOVE);
    lc_releaseReadPages(fs->fs_gfs, fs, pages, pcount);
}

/* Flush a cluster of pages */
void
lc_flushPageCluster(struct gfs *gfs, struct fs *fs,
                    struct page *head, uint64_t count) {
    struct page *page = head;
    struct iovec *iovec;
    uint64_t block = 0;
    uint64_t i, j;

    if (count == 1) {
        block = page->p_block;
        lc_writeBlock(gfs, fs, page->p_data, block);
    } else {
        iovec = alloca(count * sizeof(struct iovec));

        /* Issue the I/O in block order */
        for (i = 0, j = count - 1; i < count; i++, j--) {
            iovec[j].iov_base = page->p_data;
            iovec[j].iov_len = LC_BLOCK_SIZE;
            assert((i == 0) || (block == (page->p_block + 1)));
            block = page->p_block;
            page = page->p_dnext;
        }
        assert(page == NULL);
        assert(block != 0);
        lc_writeBlocks(gfs, fs, iovec, count, block);
    }

    /* Release the pages after writing */
    lc_releasePages(gfs, fs, head);
}

/* Add a page to the file system dirty list for writeback */
static void
lc_addPageForWriteBack(struct gfs *gfs, struct fs *fs, struct page *head,
                        struct page *tail, uint64_t pcount) {
    uint64_t block, count = 0;
    struct page *page = NULL;

    assert(count < LC_CLUSTER_SIZE);
    block = tail->p_block;
    pthread_mutex_lock(&fs->fs_plock);

    /* XXX This could happen when metadata and userdata are flushed
     * concurrently OR files flushed concurrently.
     */
    if (fs->fs_dpages && (block != (fs->fs_dpages->p_block + 1))) {
        //lc_printf("Not contigous, block %ld previous block %ld count %ld\n", block, fs->fs_dpages->p_block, fs->fs_dpcount);
        page = fs->fs_dpages;
        fs->fs_dpages = NULL;
        count = fs->fs_dpcount;
        fs->fs_dpcount = 0;
    }
    tail->p_dnext = fs->fs_dpages;
    fs->fs_dpages = head;
    fs->fs_dpcount += pcount;
    if (fs->fs_dpcount >= LC_CLUSTER_SIZE) {
        page = fs->fs_dpages;
        fs->fs_dpages = NULL;
        count = fs->fs_dpcount;
        fs->fs_dpcount = 0;
    }
    pthread_mutex_unlock(&fs->fs_plock);
    if (count) {
        lc_flushPageCluster(gfs, fs, page, count);
    }
}

/* Flush dirty pages of a file system before unmounting it */
void
lc_flushDirtyPages(struct gfs *gfs, struct fs *fs) {
    struct page *page;
    uint64_t count;

    if (fs->fs_dpcount && !fs->fs_removed) {
        pthread_mutex_lock(&fs->fs_plock);
        page = fs->fs_dpages;
        fs->fs_dpages = NULL;
        count = fs->fs_dpcount;
        fs->fs_dpcount = 0;
        pthread_mutex_unlock(&fs->fs_plock);
        if (count) {
            lc_flushPageCluster(gfs, fs, page, count);
        }
    }
}

/* Invalidate dirty pages */
void
lc_invalidateDirtyPages(struct gfs *gfs, struct fs *fs) {
    struct page *page;

    if (fs->fs_dpcount) {
        pthread_mutex_lock(&fs->fs_plock);
        page = fs->fs_dpages;
        fs->fs_dpages = NULL;
        fs->fs_dpcount = 0;
        pthread_mutex_unlock(&fs->fs_plock);
        lc_releasePages(gfs, fs, page);
    }
}

/* Flush dirty pages of an inode */
void
lc_flushPages(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    uint64_t count = 0, bcount = 0, start, end = 0, fcount = 0;
    struct page *page, *dpage = NULL, *tpage = NULL;
    bool single = true, ended = false;
    uint64_t i, lpage, pcount, block;
    char *pdata;

    assert(S_ISREG(inode->i_stat.st_mode));
    assert(!inode->i_shared);
    if ((inode->i_page == NULL) || (inode->i_stat.st_size == 0)) {
        assert(inode->i_page == NULL);
        return;
    }
    lpage = (inode->i_stat.st_size + LC_BLOCK_SIZE - 1) / LC_BLOCK_SIZE;
    assert(lpage < inode->i_pcount);

    /* Count the dirty pages */
    start = lpage;
    for (i = 0; i <= lpage; i++) {
        if (inode->i_page[i].dp_data) {
            if (ended) {
                single = false;
            }
            bcount++;
            if (i < start) {
                start = i;
            }
            end = i;
        } else {
            if ((i < inode->i_extentLength) ||
                ((i < inode->i_bcount) && inode->i_bmap[i])) {
                single = false;
            }
            ended = true;
        }
    }
    assert(bcount);

    /* XXX Deal with a fragmented file system by allocating smaller chunks */
    block = lc_blockAlloc(fs, bcount, false);

    /* Check if file has a single extent */
    if (single) {
        if (inode->i_extentLength) {
            lc_freeLayerMetaBlocks(fs, inode->i_extentBlock,
                                   inode->i_extentLength);
        } else if (inode->i_bmap) {
            for (i = 0; i < inode->i_bcount; i++) {
                if (inode->i_bmap[i]) {
                    lc_freeLayerMetaBlocks(fs, inode->i_bmap[i], 1);
                }
            }
            free(inode->i_bmap);
            inode->i_bmap = NULL;
            inode->i_bcount = 0;
        }
        inode->i_extentBlock = block;
        inode->i_extentLength = bcount;
    } else {
        if (inode->i_extentLength) {
            lc_expandBmap(inode);
        }
        lc_inodeBmapAlloc(inode);
    }

    /* Queue the pages for flushing */
    for (i = start; i <= end; i++) {
        pdata = lc_removeDirtyPage(inode, i, false);
        if (pdata) {
            page = lc_getPageNew(gfs, fs, block + count, pdata);
            if (tpage == NULL) {
                tpage = page;
            }
            page->p_dnext = dpage;
            dpage = page;
            if (!single) {
                if (inode->i_bmap[i]) {
                    lc_freeLayerMetaBlocks(fs, inode->i_bmap[i], 1);
                }
                lc_inodeBmapAdd(inode, i, block + count);
            }
            count++;
            fcount++;
            if (fs->fs_dpcount &&
                ((fcount + fs->fs_dpcount) >= LC_CLUSTER_SIZE)) {
                lc_addPageForWriteBack(gfs, fs, dpage, tpage, fcount);
                dpage = NULL;
                tpage = NULL;
                fcount = 0;
            } else if (fcount >= LC_CLUSTER_SIZE) {
                if (fs->fs_dpcount) {
                    lc_flushDirtyPages(gfs, fs);
                }
                lc_flushPageCluster(gfs, fs, page, fcount);
                dpage = NULL;
                tpage = NULL;
                fcount = 0;
            }
        }
    }
    if (fcount) {
        lc_addPageForWriteBack(gfs, fs, dpage, tpage, fcount);
    }
    assert(bcount == count);
    if (count) {
        pcount = __sync_fetch_and_sub(&inode->i_fs->fs_pcount, count);
        assert(pcount >= count);
    }

    /* Free dirty page list as all pages are in block cache */
    if (inode->i_page) {
        free(inode->i_page);
        inode->i_page = NULL;
        inode->i_pcount = 0;
    }
}

/* Truncate pages beyond the new size of the file */
void
lc_truncPages(struct inode *inode, off_t size, bool remove) {
    uint64_t pg = size / LC_BLOCK_SIZE, lpage;
    uint64_t i, bcount = 0, pcount;
    struct fs *fs = inode->i_fs;
    bool truncated = false;
    struct dpage *dpage;
    int freed = 0;
    off_t poffset;
    char *pdata;

    /* If nothing to truncate, return */
    if ((inode->i_bmap == NULL) && (inode->i_pcount == 0) &&
        (inode->i_extentLength == 0)) {
        assert(inode->i_stat.st_blocks == 0);
        assert(inode->i_stat.st_size == 0);
        assert(inode->i_bcount == 0);
        assert(inode->i_pcount == 0);
        assert(inode->i_page == NULL);
        assert(!inode->i_shared);
        inode->i_pcache = true;
        return;
    }

    /* Copy bmap list before changing it */
    if (inode->i_shared) {
        if (size == 0) {
            if (remove) {
                inode->i_stat.st_blocks = 0;
                inode->i_shared = false;
                inode->i_pcache = true;
                inode->i_extentBlock = 0;
                inode->i_extentLength = 0;
            }
            inode->i_page = NULL;
            inode->i_pcount = 0;
            inode->i_bcount = 0;
            inode->i_bmap = NULL;
            return;
        }
        lc_copyBmap(inode);
    }
    assert(!inode->i_shared);

    /* Take care of files with single extent */
    lpage = (inode->i_stat.st_size + LC_BLOCK_SIZE - 1) / LC_BLOCK_SIZE;
    if (remove && inode->i_extentLength) {
        assert(inode->i_bcount == 0);
        assert(inode->i_pcount == 0);
        if (size % LC_BLOCK_SIZE) {
            lc_expandBmap(inode);
        } else {
            if (inode->i_extentLength > pg) {
                bcount = inode->i_extentLength - pg;
                lc_blockFree(fs, inode->i_extentBlock + pg, bcount);
                inode->i_extentLength = pg;
            }
            if (inode->i_extentLength == 0) {
                inode->i_extentBlock = 0;
            }
        }
    }

    /* Remove blockmap entries past the new size */
    if (remove && inode->i_bcount) {
        assert(inode->i_stat.st_blocks <= inode->i_bcount);
        for (i = pg; i < inode->i_bcount; i++) {
            if (inode->i_bmap[i] == 0) {
                continue;
            }
            if ((pg == i) && ((size % LC_BLOCK_SIZE) != 0)) {

                /* If a page is partially truncated, keep it */
                poffset = size % LC_BLOCK_SIZE;
                lc_inodeAllocPages(inode);
                dpage = lc_findDirtyPage(inode, pg);
                if (dpage->dp_data == NULL) {
                    malloc_aligned((void **)&dpage->dp_data);
                    __sync_add_and_fetch(&fs->fs_pcount, 1);
                    dpage->dp_poffset = 0;
                    dpage->dp_psize = 0;
                } else {
                    if ((dpage->dp_poffset + dpage->dp_psize) > poffset) {
                        if (dpage->dp_poffset >= poffset) {
                            dpage->dp_poffset = 0;
                            dpage->dp_psize = 0;
                        } else {
                            dpage->dp_psize = poffset - dpage->dp_poffset;
                        }
                    }
                }
                truncated = true;
            } else {
                /* XXX Try to coalesce this */
                lc_blockFree(fs, inode->i_bmap[i], 1);
                inode->i_bmap[i] = 0;
                bcount++;
            }
        }
    }

    /* Remove dirty pages past the new size from the dirty list */
    if (inode->i_pcount) {
        assert(lpage < inode->i_pcount);
        for (i = pg; i <= lpage; i++) {
            dpage = lc_findDirtyPage(inode, i);
            pdata = dpage->dp_data;
            if (pdata == NULL) {
                continue;
            }
            if ((pg == i) && ((size % LC_BLOCK_SIZE) != 0)) {

                /* If a page is partially truncated, keep it */
                if (!truncated) {
                    poffset = size % LC_BLOCK_SIZE;
                    if ((dpage->dp_poffset + dpage->dp_psize) > poffset) {
                        if (dpage->dp_poffset >= poffset) {
                            dpage->dp_poffset = 0;
                            dpage->dp_psize = 0;
                        } else {
                            dpage->dp_psize = poffset - dpage->dp_poffset;
                        }
                    }
                }
            } else {
                lc_removeDirtyPage(inode, i, true);
                freed++;
            }
        }
    }
    if (freed) {
        pcount = __sync_fetch_and_sub(&fs->fs_pcount, freed);
        assert(pcount >= freed);
    }

    /* Update inode blocks while truncating the file */
    if (remove) {
        assert(inode->i_stat.st_blocks >= bcount);
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
    }
}
