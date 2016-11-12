#include "includes.h"

static char dfs_zPage[DFS_BLOCK_SIZE];

#define DFS_PAGECACHE_SIZE  32
#define DFS_CLUSTER_SIZE    256
#define DFS_PAGE_MAX        200000

/* Return the hash number for the block number provided */
static inline int
dfs_pageBlockHash(uint64_t block) {
    assert(block);
    assert(block != DFS_INVALID_BLOCK);
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

/* True if page is on the freelist */
static inline bool
dfs_pageIsFree(struct gfs *gfs, struct page *page) {
    return (page->p_fnext != NULL) ||
           (page->p_fprev != NULL) ||
           (gfs->gfs_pfirst == page);
}

/* Allocate a new page */
static struct page *
dfs_newPage(struct gfs *gfs) {
    struct page *page = malloc(sizeof(struct page));

    page->p_data = NULL;
    page->p_pcache = NULL;
    page->p_block = DFS_INVALID_BLOCK;
    page->p_refCount = 0;
    page->p_fprev = NULL;
    page->p_fnext = NULL;
    page->p_cnext = NULL;
    page->p_dnext = NULL;
    pthread_mutex_init(&page->p_lock, NULL);
    __sync_add_and_fetch(&gfs->gfs_pcount, 1);
    //dfs_printf("new page %p gfs->gfs_pcount %ld\n", page, gfs->gfs_pcount);
    return page;
}

/* Free a page */
static void
dfs_freePage(struct gfs *gfs, struct page *page) {
    assert(page->p_refCount == 0);
    assert(page->p_block == DFS_INVALID_BLOCK);
    assert(page->p_cnext == NULL);
    assert(!dfs_pageIsFree(gfs, page));
    assert(page->p_dnext == NULL);

    if (page->p_data) {
        free(page->p_data);
    }
    pthread_mutex_destroy(&page->p_lock);
    free(page);
}

/* Add a page to free list */
static void
dfs_insertFreeList(struct gfs *gfs, struct page *page) {
    assert(!dfs_pageIsFree(gfs, page));

    if (page->p_block == DFS_INVALID_BLOCK) {
        page->p_fprev = gfs->gfs_plast;
        if (page->p_fprev != NULL) {
            page->p_fprev->p_fnext = page;
        }
        gfs->gfs_plast = page;
        if (gfs->gfs_pfirst == NULL) {
            gfs->gfs_pfirst = page;
        }
    } else {
        page->p_fnext = gfs->gfs_pfirst;
        if (page->p_fnext != NULL) {
            page->p_fnext->p_fprev = page;
        }
        gfs->gfs_pfirst = page;
        if (gfs->gfs_plast == NULL) {
            gfs->gfs_plast = page;
        }
    }
}

/* Remove a page from the free list */
static void
dfs_removeFreelist(struct gfs *gfs, struct page *page) {
    if (page->p_fprev) {
        page->p_fprev->p_fnext = page->p_fnext;
    }
    if (page->p_fnext) {
        page->p_fnext->p_fprev = page->p_fprev;
    }
    if (gfs->gfs_pfirst == page) {
        gfs->gfs_pfirst = page->p_fnext;
    }
    if (gfs->gfs_plast == page) {
        gfs->gfs_plast = page->p_fprev;
    }
    page->p_fprev = NULL;
    page->p_fnext = NULL;
}

/* Release a page */
void
dfs_releasePage(struct gfs *gfs, struct page *page) {
    assert(!dfs_pageIsFree(gfs, page));
    assert(page->p_data);

    pthread_mutex_lock(&page->p_lock);
    assert(page->p_refCount > 0);
    if (page->p_refCount > 1) {
        page->p_refCount--;
        pthread_mutex_unlock(&page->p_lock);
        return;
    }
    if (pthread_mutex_trylock(&gfs->gfs_plock)) {
        pthread_mutex_unlock(&page->p_lock);
        pthread_mutex_lock(&gfs->gfs_plock);
        pthread_mutex_lock(&page->p_lock);
    }
    assert(page->p_refCount > 0);
    page->p_refCount--;

    /* Insert to the freelist on last release */
    if (page->p_refCount == 0) {
        dfs_insertFreeList(gfs, page);
    }
    pthread_mutex_unlock(&gfs->gfs_plock);
    pthread_mutex_unlock(&page->p_lock);
}

/* Free pages in free list */
void
dfs_destroyFreePages(struct gfs *gfs) {
    uint64_t count = 0, pcount;
    struct page *page;

    pthread_mutex_lock(&gfs->gfs_plock);
    while ((page = gfs->gfs_pfirst)) {
        dfs_removeFreelist(gfs, page);
        dfs_freePage(gfs, page);
        count++;
    }
    pthread_mutex_unlock(&gfs->gfs_plock);
    assert(gfs->gfs_pfirst == NULL);
    assert(gfs->gfs_plast == NULL);
    if (count) {
        pcount = __sync_fetch_and_sub(&gfs->gfs_pcount, count);
        assert(pcount >= count);
    }
    assert(gfs->gfs_pcount == 0);
}

/* Return a page from the free list or allocated new */
static struct page *
dfs_findFreePage(struct gfs *gfs, struct fs *fs) {
    struct page *page = NULL, *cpage, *prev;
    struct pcache *pcache;
    int hash;

    if ((gfs->gfs_pcount > DFS_PAGE_MAX) && gfs->gfs_plast) {
        pthread_mutex_lock(&gfs->gfs_plock);
        page = gfs->gfs_plast;
        while (page) {
            if (page->p_refCount) {
                page = page->p_fprev;
                continue;
            }
            pthread_mutex_lock(&page->p_lock);
            if (page->p_refCount) {
                page = page->p_fprev;
                pthread_mutex_unlock(&page->p_lock);
                continue;
            }

            //dfs_printf("Reusing page with block %ld\n", page->p_block);
            /* Take the page out of the block hash */
            if (page->p_block != DFS_INVALID_BLOCK) {
                prev = NULL;
                pcache = page->p_pcache;
                hash = dfs_pageBlockHash(page->p_block);

                /* XXX Avoid skipping this page due to cache lock */
                if (pthread_mutex_trylock(&pcache[hash].pc_lock)) {
                    page = page->p_fprev;
                    pthread_mutex_unlock(&page->p_lock);
                    continue;
                }
                cpage = pcache[hash].pc_head;
                while (cpage) {
                    if (cpage == page) {
                        if (prev == NULL) {
                            pcache[hash].pc_head = page->p_cnext;
                        } else {
                            prev->p_cnext = page->p_cnext;
                        }
                        page->p_pcache = NULL;
                        page->p_block = DFS_INVALID_BLOCK;
                        page->p_cnext = NULL;
                        if (page->p_data) {
                            __sync_sub_and_fetch(&pcache->pc_pcount, 1);
                        }
                        break;
                    }
                    prev = cpage;
                    cpage = cpage->p_cnext;
                }
                pthread_mutex_unlock(&pcache[hash].pc_lock);
            }

            /* Take the page out of the free list */
            dfs_removeFreelist(gfs, page);
            pthread_mutex_unlock(&page->p_lock);
            assert(page->p_refCount == 0);
            assert(page->p_block == DFS_INVALID_BLOCK);
            assert(page->p_cnext == NULL);
            assert(page->p_dnext == NULL);
            break;
        }
        pthread_mutex_unlock(&gfs->gfs_plock);
        /* XXX reuse this memory */
        if (page && page->p_data) {
            free(page->p_data);
            page->p_data = NULL;
        }
    }
    if (page == NULL) {
        page = dfs_newPage(gfs);
    } else {
        __sync_add_and_fetch(&gfs->gfs_precycle, 1);
    }
    return page;
}

/* Lookup a page in the block hash */
static struct page *
dfs_getPage(struct fs *fs, uint64_t block, bool read) {
    int hash = dfs_pageBlockHash(block);
    struct pcache *pcache = fs->fs_pcache;
    struct page *page, *new = NULL;
    struct gfs *gfs = fs->fs_gfs;
    bool hit = false;

    assert(block);
    assert(block != DFS_PAGE_HOLE);

retry:
    pthread_mutex_lock(&pcache[hash].pc_lock);
    page = pcache[hash].pc_head;
    while (page) {
        if (page->p_block == block) {
            hit = true;
            break;
        }
        page = page->p_cnext;
    }
    if ((page == NULL) && new) {
        page = new;
        page->p_pcache = pcache;
        page->p_block = block;
        page->p_cnext = pcache[hash].pc_head;
        pcache[hash].pc_head = page;
        if (page->p_data) {
            __sync_add_and_fetch(&pcache->pc_pcount, 1);
        }
        new = NULL;
    }
    if (page) {
        pthread_mutex_lock(&page->p_lock);
        page->p_refCount++;
    }
    pthread_mutex_unlock(&pcache[hash].pc_lock);

    if (page == NULL) {
        new = dfs_findFreePage(gfs, fs);
        goto retry;
    }

    if (page->p_refCount == 1) {
        /* Take the page out of the free list */
        if (dfs_pageIsFree(gfs, page)) {
            pthread_mutex_lock(&gfs->gfs_plock);
            dfs_removeFreelist(gfs, page);
            if (new) {
                dfs_insertFreeList(gfs, new);
            }
            pthread_mutex_unlock(&gfs->gfs_plock);
            new = NULL;
        }

        /* If page is missing data, read from disk */
        if (read && (page->p_data == NULL)) {
            page->p_data = dfs_readBlock(gfs, fs, block);
            pthread_mutex_unlock(&page->p_lock);
            __sync_add_and_fetch(&pcache->pc_pcount, 1);
        } else {
            pthread_mutex_unlock(&page->p_lock);
        }
    } else {
        pthread_mutex_unlock(&page->p_lock);
    }
    if (new) {
        pthread_mutex_lock(&gfs->gfs_plock);
        dfs_insertFreeList(gfs, new);
        pthread_mutex_unlock(&gfs->gfs_plock);
    }
    assert(!dfs_pageIsFree(gfs, page));
    assert(!read || page->p_data);
    assert(read || (page->p_data == NULL));
    assert(page->p_block == block);
    if (hit) {
        __sync_add_and_fetch(&gfs->gfs_phit, 1);
    } else if (read) {
        __sync_add_and_fetch(&gfs->gfs_pmissed, 1);
    }
    return page;
}

/* Insert a page into page hash */
static struct page *
dfs_getPageNew(struct gfs *gfs, struct fs *fs, uint64_t block, char *data) {
    struct page *page = dfs_getPage(fs, block, false);

    if (page->p_data) {
        free(page->p_data);
        page->p_data = NULL;
        __sync_sub_and_fetch(&page->p_pcache->pc_pcount, 1);
    }
    page->p_data = data;
    return page;
}

/* Remove pages from page cache and free the hash table */
void
dfs_destroy_pages(struct pcache *pcache) {
    uint64_t count = 0;
    struct page *page;
    int i;

    for (i = 0; i < DFS_PCACHE_SIZE; i++) {
        pthread_mutex_lock(&pcache[i].pc_lock);
        while ((page = pcache[i].pc_head)) {
            pthread_mutex_lock(&page->p_lock);
            assert(page->p_refCount == 0);
            if (page->p_data) {
                count++;
            }
            pcache[i].pc_head = page->p_cnext;
            page->p_pcache = NULL;
            page->p_block = DFS_INVALID_BLOCK;
            page->p_cnext = NULL;

            /* XXX Move to the tail of the free list */
            pthread_mutex_unlock(&page->p_lock);
        }
        assert(pcache[i].pc_head == NULL);
        pthread_mutex_unlock(&pcache[i].pc_lock);
        pthread_mutex_destroy(&pcache[i].pc_lock);
    }
    if (count) {
        __sync_sub_and_fetch(&pcache->pc_pcount, count);
    }
    assert(pcache->pc_pcount == 0);
    free(pcache);
}

/* Add a new dirty page */
static inline void
dfs_insertDirtyPage(struct inode *inode, uint64_t pg, char *pdata) {
    assert(pg < inode->i_pcount);
    assert(inode->i_page[pg] == NULL);
    inode->i_page[pg] = pdata;
}

/* Return the requested page if allocated already */
static inline char *
dfs_findDirtyPage(struct inode *inode, uint64_t pg) {
    return (pg < inode->i_pcount) ? inode->i_page[pg] : NULL;
}

/* Remove a dirty page */
static inline void
dfs_removeDirtyPage(struct inode *inode, uint64_t pg) {
    char *pdata = inode->i_page[pg];

    free(pdata);
    inode->i_page[pg] = NULL;
}

/* Add the page to bufvec */
static inline void
dfs_updateVec(char *pdata, struct fuse_bufvec *bufv,
              off_t poffset, size_t psize) {
    bufv->buf[bufv->count].mem = &pdata[poffset];
    bufv->buf[bufv->count].size = psize;
    bufv->count++;
}

/* Allocate/extend inode page table */
static void
dfs_inodeAllocPages(struct inode *inode) {
    uint64_t lpage, count, size, tsize;
    char **page;

    assert(!inode->i_shared);
    lpage = (inode->i_stat.st_size + DFS_BLOCK_SIZE - 1) / DFS_BLOCK_SIZE;
    if (inode->i_pcount <= lpage) {
        count = inode->i_pcount ? (inode->i_pcount * 2) :
                                  (lpage ? (lpage + 1) : DFS_PAGECACHE_SIZE);
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
dfs_addPage(struct inode *inode, uint64_t pg, off_t poffset, size_t psize,
            int *incr, struct fuse_bufvec *bufv) {
    struct page *bpage;
    char *data, *pdata;
    bool new = false;
    uint64_t block;

    assert(pg < inode->i_pcount);
    assert(!inode->i_shared);
    pdata = dfs_findDirtyPage(inode, pg);
    if (pdata == NULL) {
        if ((poffset != 0) || ((poffset + psize) != DFS_BLOCK_SIZE)) {
            block = dfs_inodeBmapLookup(inode, pg);
            if (block != DFS_PAGE_HOLE) {
                bpage = dfs_getPage(inode->i_fs, block, true);
                data = bpage->p_data;
            } else {
                data = NULL;
            }
        } else {
            data = NULL;
        }
        posix_memalign((void **)&pdata, DFS_BLOCK_SIZE, DFS_BLOCK_SIZE);
        *incr = (*incr) + 1;
        if (poffset != 0) {
            if (data) {
                memcpy(pdata, data, poffset);
            } else {
                memset(pdata, 0, poffset);
            }
        }
        if ((poffset + psize) != DFS_BLOCK_SIZE) {
            if (data) {
                memcpy(&pdata[(poffset + psize)], data,
                    DFS_BLOCK_SIZE - (poffset + psize));
            } else {
                memset(&pdata[(poffset + psize)], 0,
                    DFS_BLOCK_SIZE - (poffset + psize));
            }
        }
        if (data) {
            dfs_releasePage(inode->i_fs->fs_gfs, bpage);
        }
        dfs_insertDirtyPage(inode, pg, pdata);
    }
    dfs_updateVec(pdata, bufv, poffset, psize);
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
    if (inode->i_extentLength) {
        dfs_expandBmap(inode);
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
int
dfs_readPages(struct inode *inode, off_t soffset, off_t endoffset,
              struct page **pages, struct fuse_bufvec *bufv) {
    uint64_t block, pg = soffset / DFS_BLOCK_SIZE, pcount = 0, i = 0;
    off_t poffset, off = soffset, roff = 0;
    size_t psize, rsize = endoffset - off;
    struct fs *fs = inode->i_fs;
    struct page *page = NULL;
    char *data;

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

        data = dfs_findDirtyPage(inode, pg);
        if (data == NULL) {
            block = dfs_inodeBmapLookup(inode, pg);
            if (block == DFS_PAGE_HOLE) {
                bufv->buf[i].mem = dfs_zPage;
            } else {
                page = dfs_getPage(fs, block, true);
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
static int
dfs_flushPageCluster(struct gfs *gfs, struct fs *fs,
                     struct page *head, uint64_t count) {
    struct page *page = head;
    struct iovec *iovec;
    uint64_t block = 0;
    uint64_t i, j;

    if (count == 1) {
        block = page->p_block;
        dfs_writeBlock(gfs, fs, page->p_data, block);
    } else {
        iovec = alloca(count * sizeof(struct iovec));
        for (i = 0, j = count - 1; i < count; i++, j--) {
            iovec[j].iov_base = page->p_data;
            iovec[j].iov_len = DFS_BLOCK_SIZE;
            assert((i == 0) || (block == (page->p_block + 1)));
            block = page->p_block;
            page = page->p_dnext;
        }
        assert(page == NULL);
        assert(block != 0);
        dfs_writeBlocks(gfs, fs, iovec, count, block);
    }
    page = head;
    while (page) {
        dfs_releasePage(gfs, page);
        head = page->p_dnext;
        page->p_dnext = NULL;
        page = head;
    }
    //dfs_printf("Flushed %ld pages to block %ld\n", count, block);
    return count;
}

/* Add a page to the file system dirty list for writeback */
static void
dfs_addPageForWriteBack(struct gfs *gfs, struct fs *fs, struct page *head,
                        struct page *tail, uint64_t pcount) {
    uint64_t block, count = 0;
    struct page *page;

    assert(count < DFS_CLUSTER_SIZE);
    block = tail->p_block;
    pthread_mutex_lock(&fs->fs_plock);

    /* XXX This could happen when metadata and userdata are flushed
     * concurrently OR files flushed concurrently.
     */
    if (fs->fs_dpages && (block != (fs->fs_dpages->p_block + 1))) {
        //dfs_printf("Not contigous, block %ld previous block %ld count %ld\n", block, fs->fs_dpages->p_block, fs->fs_dpcount);
        page = fs->fs_dpages;
        fs->fs_dpages = NULL;
        count = fs->fs_dpcount;
        fs->fs_dpcount = 0;
    }
    tail->p_dnext = fs->fs_dpages;
    fs->fs_dpages = head;
    fs->fs_dpcount += pcount;
    if (fs->fs_dpcount >= DFS_CLUSTER_SIZE) {
        page = fs->fs_dpages;
        fs->fs_dpages = NULL;
        count = fs->fs_dpcount;
        fs->fs_dpcount = 0;
    }
    pthread_mutex_unlock(&fs->fs_plock);
    if (count) {
        dfs_flushPageCluster(gfs, fs, page, count);
    }
}

/* Flush dirty pages of a file system before unmounting it */
void
dfs_flushDirtyPages(struct gfs *gfs, struct fs *fs) {
    struct page * page;
    uint64_t count;

    if (fs->fs_dpcount) {
        pthread_mutex_lock(&fs->fs_plock);
        page = fs->fs_dpages;
        fs->fs_dpages = NULL;
        count = fs->fs_dpcount;
        fs->fs_dpcount = 0;
        pthread_mutex_unlock(&fs->fs_plock);
        if (count) {
            dfs_flushPageCluster(gfs, fs, page, count);
        }
    }
}

/* Flush dirty pages of an inode */
void
dfs_flushPages(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    uint64_t count = 0, bcount = 0, start, end = 0, fcount = 0;
    struct page *page, *dpage = NULL, *tpage = NULL;
    bool single = true, ended = false;
    uint64_t i, lpage, pcount, block;
    char *pdata;

    assert(S_ISREG(inode->i_stat.st_mode));
    assert(!inode->i_shared);
    //dfs_printf("Flushing pages of inode %ld\n", inode->i_stat.st_ino);
    if ((inode->i_page == NULL) || (inode->i_stat.st_size == 0)) {
        return;
    }
    lpage = (inode->i_stat.st_size + DFS_BLOCK_SIZE - 1) / DFS_BLOCK_SIZE;
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
            ended = true;
        }
    }
    assert(bcount);
    block = dfs_blockAlloc(fs, bcount, false);

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
        dfs_inodeBmapAlloc(inode);
    }
    __sync_add_and_fetch(&fs->fs_pcache->pc_pcount, bcount);
    for (i = start; i <= end; i++) {
        pdata = inode->i_page[i];
        if (pdata) {
            page = dfs_getPageNew(gfs, fs, block + count, pdata);
            if (tpage == NULL) {
                tpage = page;
            }
            page->p_dnext = dpage;
            dpage = page;
            /* XXX free the old block */
            if (!single) {
                dfs_inodeBmapAdd(inode, i, block + count);
            }
            count++;
            fcount++;
            if (fs->fs_dpcount &&
                ((fcount + fs->fs_dpcount) >= DFS_CLUSTER_SIZE)) {
                dfs_addPageForWriteBack(gfs, fs, dpage, tpage, fcount);
                dpage = NULL;
                tpage = NULL;
                fcount = 0;
            } else if (fcount >= DFS_CLUSTER_SIZE) {
                if (fs->fs_dpcount) {
                    dfs_flushDirtyPages(gfs, fs);
                }
                dfs_flushPageCluster(gfs, fs, page, fcount);
                dpage = NULL;
                tpage = NULL;
                fcount = 0;
            }
        }
    }
    if (fcount) {
        dfs_addPageForWriteBack(gfs, fs, dpage, tpage, fcount);
    }
    assert(bcount == count);
    if (count) {
        pcount = __sync_fetch_and_sub(&inode->i_fs->fs_pcount, count);
        assert(pcount >= count);
    }
    if (inode->i_page) {
        free(inode->i_page);
        inode->i_page = NULL;
        inode->i_pcount = 0;
    }
}

/* Truncate pages beyond the new size of the file */
uint64_t
dfs_truncPages(struct inode *inode, off_t size, bool remove) {
    uint64_t pg = size / DFS_BLOCK_SIZE, lpage;
    uint64_t i, bcount = 0, tcount = 0, pcount;
    bool truncated = false;
    struct page *bpage;
    int freed = 0;
    off_t poffset;
    char *pdata;

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
            inode->i_extentBlock = 0;
            inode->i_extentLength = 0;
            return 0;
        }
        dfs_copyBmap(inode);
    }
    assert(!inode->i_shared);
    lpage = (inode->i_stat.st_size + DFS_BLOCK_SIZE - 1) / DFS_BLOCK_SIZE;
    if (inode->i_extentLength) {
        assert(inode->i_bcount == 0);
        assert(inode->i_pcount == 0);
        if (size % DFS_BLOCK_SIZE) {
            dfs_expandBmap(inode);
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
                pdata = dfs_findDirtyPage(inode, pg);
                if (pdata == NULL) {
                    posix_memalign((void **)&pdata,
                                   DFS_BLOCK_SIZE, DFS_BLOCK_SIZE);
                    __sync_add_and_fetch(&inode->i_fs->fs_pcount, 1);
                    bpage = dfs_getPage(inode->i_fs, inode->i_bmap[i], true);
                    memcpy(pdata, bpage->p_data, poffset);
                    dfs_releasePage(inode->i_fs->fs_gfs, bpage);
                    dfs_insertDirtyPage(inode, pg, pdata);
                }
                memset(&pdata[poffset], 0, DFS_BLOCK_SIZE - poffset);
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
            pdata = inode->i_page[i];
            if (pdata == NULL) {
                continue;
            }
            if ((pg == i) && ((size % DFS_BLOCK_SIZE) != 0)) {

                /* If a page is partially truncated, keep it */
                if (!truncated) {
                    poffset = size % DFS_BLOCK_SIZE;
                    memset(&pdata[poffset], 0,
                           DFS_BLOCK_SIZE - poffset);
                }
            } else {
                dfs_removeDirtyPage(inode, i);
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
