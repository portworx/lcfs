#include "includes.h"

static char lc_zPage[LC_BLOCK_SIZE];

#define LC_PAGECACHE_SIZE  32
#define LC_CLUSTER_SIZE    256
#define LC_PAGE_MAX        1200000

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

/* Release pages */
void
lc_releaseReadPages(struct gfs *gfs, struct fs *fs,
                     struct page **pages, uint64_t pcount) {
    uint64_t i;

    for (i = 0; i < pcount; i++) {
        lc_releasePage(gfs, fs, pages[i], true);
    }
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
    assert(page->p_refCount >= 1);
    assert(!read || page->p_data);
    assert(!read || page->p_dvalid);
    assert(read || !page->p_dvalid);
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
            assert(page->p_refCount == 0);
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

/* Add a new dirty page */
static inline void
lc_insertDirtyPage(struct inode *inode, uint64_t pg, char *pdata) {
    assert(pg < inode->i_pcount);
    assert(inode->i_page[pg] == NULL);
    inode->i_page[pg] = pdata;
}

/* Return the requested page if allocated already */
static inline char *
lc_findDirtyPage(struct inode *inode, uint64_t pg) {
    return (pg < inode->i_pcount) ? inode->i_page[pg] : NULL;
}

/* Remove a dirty page */
static inline void
lc_removeDirtyPage(struct inode *inode, uint64_t pg) {
    char *pdata = inode->i_page[pg];

    free(pdata);
    inode->i_page[pg] = NULL;
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
    char **page;

    assert(!inode->i_shared);
    lpage = (inode->i_stat.st_size + LC_BLOCK_SIZE - 1) / LC_BLOCK_SIZE;
    if (inode->i_pcount <= lpage) {
        count = inode->i_pcount ? (inode->i_pcount * 2) :
                                  (lpage ? (lpage + 1) : LC_PAGECACHE_SIZE);
        while (count <= lpage) {
            count *= 2;
        }
        tsize = count * sizeof(char *);
        page = malloc(tsize);
        if (inode->i_pcount) {
            size = inode->i_pcount * sizeof(char *);
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
lc_addPage(struct inode *inode, uint64_t pg, off_t poffset, size_t psize,
            int *incr, struct fuse_bufvec *bufv) {
    struct page *bpage;
    char *data, *pdata;
    bool new = false;
    uint64_t block;

    assert(pg < inode->i_pcount);
    assert(!inode->i_shared);

    /* Check if a dirty page exists already */
    pdata = lc_findDirtyPage(inode, pg);
    if (pdata == NULL) {

        /* If the page is written partially, check if page already exists */
        if ((poffset != 0) || ((poffset + psize) != LC_BLOCK_SIZE)) {
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
        posix_memalign((void **)&pdata, LC_BLOCK_SIZE, LC_BLOCK_SIZE);
        *incr = (*incr) + 1;

        /* Copy partial data or zero out partial page if page is new */
        if (poffset != 0) {
            if (data) {
                memcpy(pdata, data, poffset);
            } else {
                memset(pdata, 0, poffset);
            }
        }
        if ((poffset + psize) != LC_BLOCK_SIZE) {
            if (data) {
                memcpy(&pdata[(poffset + psize)], data,
                    LC_BLOCK_SIZE - (poffset + psize));
            } else {
                memset(&pdata[(poffset + psize)], 0,
                    LC_BLOCK_SIZE - (poffset + psize));
            }
        }
        if (data) {
            lc_releasePage(inode->i_fs->fs_gfs, inode->i_fs, bpage, true);
        }
        lc_insertDirtyPage(inode, pg, pdata);
    }
    lc_updateVec(pdata, bufv, poffset, psize);
    return new;
}

/* Update pages of a file with provided data */
int
lc_addPages(struct inode *inode, off_t off, size_t size,
             struct fuse_bufvec *bufv, struct fuse_bufvec *dst) {
    size_t wsize = size, psize;
    int count = 0, added = 0;
    uint64_t page, spage;
    off_t poffset;

    assert(S_ISREG(inode->i_stat.st_mode));

    /* Copy page headers if page chain is shared */
    if (inode->i_shared) {
        lc_copyBmap(inode);
    }
    if (inode->i_extentLength) {
        lc_expandBmap(inode);
    }
    lc_inodeAllocPages(inode);
    spage = off / LC_BLOCK_SIZE;
    page = spage;

    /* Break the down the write into pages and link those to the file */
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
        count += lc_addPage(inode, page, poffset, psize, &added, dst);
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
int
lc_readPages(struct inode *inode, off_t soffset, off_t endoffset,
              struct page **pages, struct fuse_bufvec *bufv) {
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
        data = lc_findDirtyPage(inode, pg);
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
    return pcount;
}

/* Flush a cluster of pages */
static void
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
    page = head;
    while (page) {
        head = page->p_dnext;
        page->p_dnext = NULL;
        lc_releasePage(gfs, fs, page, false);
        page = head;
    }
}

/* Add a page to the file system dirty list for writeback */
static void
lc_addPageForWriteBack(struct gfs *gfs, struct fs *fs, struct page *head,
                        struct page *tail, uint64_t pcount) {
    uint64_t block, count = 0;
    struct page *page;

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

    if (fs->fs_dpcount) {
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
    struct page *page, *next;

    if (fs->fs_dpcount) {
        pthread_mutex_lock(&fs->fs_plock);
        page = fs->fs_dpages;
        fs->fs_dpages = NULL;
        fs->fs_dpcount = 0;
        pthread_mutex_unlock(&fs->fs_plock);
        while (page) {
            next = page->p_dnext;
            page->p_dnext = NULL;
            lc_releasePage(gfs, fs, page, false);
            page = next;
        }
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
        return;
    }
    lpage = (inode->i_stat.st_size + LC_BLOCK_SIZE - 1) / LC_BLOCK_SIZE;
    assert(lpage < inode->i_pcount);

    /* Count the dirty pages */
    start = lpage;
    for (i = 0; i <= lpage; i++) {
        if (inode->i_page[i]) {
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
    block = lc_blockAlloc(fs, bcount, false);

    /* Check if file has a single extent */
    if (single) {
        inode->i_extentBlock = block;
        inode->i_extentLength = bcount;
        if (inode->i_bmap) {
            free(inode->i_bmap);
            inode->i_bmap = NULL;
            inode->i_bcount = 0;
        }
    } else {
        if (inode->i_extentLength) {
            lc_expandBmap(inode);
        }
        lc_inodeBmapAlloc(inode);
    }

    /* Queue the pages for flushing */
    for (i = start; i <= end; i++) {
        pdata = inode->i_page[i];
        if (pdata) {
            page = lc_getPageNew(gfs, fs, block + count, pdata);
            if (tpage == NULL) {
                tpage = page;
            }
            page->p_dnext = dpage;
            dpage = page;
            /* XXX free the old block */
            if (!single) {
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
uint64_t
lc_truncPages(struct inode *inode, off_t size, bool remove) {
    uint64_t pg = size / LC_BLOCK_SIZE, lpage;
    uint64_t i, bcount = 0, tcount = 0, pcount;
    bool truncated = false;
    struct page *bpage;
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
        assert(inode->i_page == 0);
        assert(!inode->i_shared);
        inode->i_pcache = true;
        return 0;
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
            return 0;
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
                inode->i_extentLength = pg;
            }
            if (inode->i_extentLength == 0) {
                inode->i_extentBlock = 0;
            }
        }
    }

    /* Remove blockmap entries past the new size */
    if (remove && (size == 0)) {
        bcount = inode->i_stat.st_blocks;
    } else if (remove && inode->i_bcount) {
        assert(inode->i_stat.st_blocks <= inode->i_bcount);
        for (i = pg; i < inode->i_bcount; i++) {
            if (inode->i_bmap[i] == 0) {
                continue;
            }
            if ((pg == i) && ((size % LC_BLOCK_SIZE) != 0)) {

                /* If a page is partially truncated, keep it */
                poffset = size % LC_BLOCK_SIZE;
                lc_inodeAllocPages(inode);
                pdata = lc_findDirtyPage(inode, pg);
                if (pdata == NULL) {
                    posix_memalign((void **)&pdata,
                                   LC_BLOCK_SIZE, LC_BLOCK_SIZE);
                    __sync_add_and_fetch(&inode->i_fs->fs_pcount, 1);
                    bpage = lc_getPage(inode->i_fs, inode->i_bmap[i], true);
                    memcpy(pdata, bpage->p_data, poffset);
                    lc_releasePage(inode->i_fs->fs_gfs, inode->i_fs, bpage,
                                    true);
                    lc_insertDirtyPage(inode, pg, pdata);
                }
                memset(&pdata[poffset], 0, LC_BLOCK_SIZE - poffset);
                inode->i_blks++;
                truncated = true;
            } else {
                inode->i_bmap[i] = 0;
                bcount++;
            }
        }
    }

    /* Remove dirty pages past the new size from the dirty list */
    if (inode->i_pcount) {
        assert(lpage < inode->i_pcount);
        for (i = pg; i <= lpage; i++) {
            pdata = inode->i_page[i];
            if (pdata == NULL) {
                continue;
            }
            if ((pg == i) && ((size % LC_BLOCK_SIZE) != 0)) {

                /* If a page is partially truncated, keep it */
                if (!truncated) {
                    poffset = size % LC_BLOCK_SIZE;
                    memset(&pdata[poffset], 0,
                           LC_BLOCK_SIZE - poffset);
                }
            } else {
                lc_removeDirtyPage(inode, i);
                freed++;
            }
        }
    }
    if (freed) {
        pcount = __sync_fetch_and_sub(&inode->i_fs->fs_pcount, freed);
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

        /* XXX Does not work after remount */
        tcount = inode->i_blks;
        inode->i_blks = 0;
    }
    return tcount;
}
