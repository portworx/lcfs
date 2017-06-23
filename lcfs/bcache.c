#include "includes.h"

/* Return the hash number for the block number provided */
/* XXX Figure out a better hashing scheme */
static inline int
lc_pageBlockHash(struct fs *fs, uint64_t block) {
    assert(block);
    assert(block != LC_INVALID_BLOCK);
    return block % fs->fs_bcache->lb_pcacheSize;
}

/* Add a page to free list */
static void
lc_insertPageToFreeList(struct lbcache *lbcache, struct page *page) {
    if (lbcache->lb_ftail) {
        page->p_fprev = lbcache->lb_ftail;
        lbcache->lb_ftail->p_fnext = page;
    } else {
        assert(lbcache->lb_fhead == NULL);
        lbcache->lb_fhead = page;
        page->p_fprev = NULL;
    }
    lbcache->lb_ftail = page;
    page->p_fnext = NULL;
}

/* Remove a page from freelist */
static void
lc_removePageFromFreeList(struct lbcache *lbcache, struct page *page) {
    if (page->p_fprev) {
        page->p_fprev->p_fnext = page->p_fnext;
    }
    if (page->p_fnext) {
        page->p_fnext->p_fprev = page->p_fprev;
    }
    if (lbcache->lb_fhead == page) {
        lbcache->lb_fhead = page->p_fnext;
    }
    if (lbcache->lb_ftail == page) {
        lbcache->lb_ftail = page->p_fprev;
    }
    page->p_fnext = NULL;
    page->p_fprev = NULL;
}

/* Allocate a new page. Memory is counted against the base layer */
static struct page *
lc_newPage(struct gfs *gfs, struct fs *fs) {
    struct page *page = lc_malloc(fs->fs_rfs, sizeof(struct page),
                                  LC_MEMTYPE_PAGE);
    page->p_data = NULL;
    page->p_block = LC_INVALID_BLOCK;
    page->p_refCount = 1;
    page->p_hitCount = 1;
    page->p_nohash = 0;
    page->p_nofree = 0;
    page->p_cache = 0;
    page->p_nocache = 0;
    page->p_dvalid = 0;
    page->p_cnext = NULL;
    page->p_dnext = NULL;
    page->p_fnext = NULL;
    page->p_fprev = NULL;
    __sync_add_and_fetch(&fs->fs_bcache->lb_pcount, 1);
    __sync_add_and_fetch(&gfs->gfs_pcount, 1);
    return page;
}

/* Free a page */
static void
lc_freePage(struct gfs *gfs, struct fs *fs, struct page *page) {
    struct lbcache *lbcache = fs->fs_bcache;
    assert(page->p_refCount == 0);
    assert(page->p_block == LC_INVALID_BLOCK);
    assert(page->p_cnext == NULL);
    assert(page->p_dnext == NULL);

    if (!page->p_nohash) {
        pthread_mutex_lock(&lbcache->lb_flock);
        lc_removePageFromFreeList(lbcache, page);
        pthread_mutex_unlock(&lbcache->lb_flock);
    }
    assert(page->p_fprev == NULL);
    assert(page->p_fnext == NULL);
    assert(lbcache->lb_fhead != page);
    if (page->p_data && !page->p_nofree) {
        lc_freePageData(gfs, fs->fs_rfs, page->p_data);
    }
    lc_free(fs->fs_rfs, page, sizeof(struct page), LC_MEMTYPE_PAGE);
    __sync_sub_and_fetch(&fs->fs_bcache->lb_pcount, 1);
    __sync_sub_and_fetch(&gfs->gfs_pcount, 1);
}

/* Allocate and initialize page block hash table */
void
lc_bcacheInit(struct fs *fs, uint32_t count, uint32_t lcount) {
    struct lbcache *lbcache;
    pthread_mutex_t *locks;
    int i;

    lbcache = lc_malloc(fs, sizeof(struct lbcache), LC_MEMTYPE_LBCACHE);
    lbcache->lb_pcache = lc_malloc(fs, sizeof(struct pcache) * count,
                                   LC_MEMTYPE_PCACHE);
    memset(lbcache->lb_pcache, 0, sizeof(struct pcache) * count);

    /* Allocate specified number of locks */
    locks = lc_malloc(fs, sizeof(pthread_mutex_t) * lcount * 2,
                      LC_MEMTYPE_PCLOCK);
    lbcache->lb_pcacheLocks = locks;
    lbcache->lb_pioLocks = locks + lcount;
    for (i = 0; i < (lcount * 2); i++) {
        pthread_mutex_init(&locks[i], NULL);
    }
    pthread_mutex_init(&lbcache->lb_flock, NULL);
    lbcache->lb_fhead = NULL;
    lbcache->lb_ftail = NULL;
    lbcache->lb_pcacheSize = count;
    lbcache->lb_pcacheLockCount = lcount;
    lbcache->lb_pcount = 0;
    fs->fs_bcache = lbcache;
}

/* Free bcache structure */
void
lc_bcacheFree(struct fs *fs) {
    struct lbcache *lbcache = fs->fs_bcache;
    uint32_t lcount;
#ifdef LC_MUTEX_DESTROY
    pthread_mutex_t *locks;
    uint32_t lcount;
#endif

    /* Free the bcache when the base layer is deleted/unmounted */
    if (fs->fs_parent == NULL) {
        assert(lbcache->lb_fhead == NULL);
        assert(lbcache->lb_ftail == NULL);
        assert(lbcache->lb_pcount == 0);
        lc_free(fs, lbcache->lb_pcache,
                sizeof(struct pcache) * lbcache->lb_pcacheSize,
                LC_MEMTYPE_PCACHE);
        lcount = lbcache->lb_pcacheLockCount * 2;
#ifdef LC_MUTEX_DESTROY
        locks = lbcache->lb_pcacheLocks;
        for (i = 0; i < lcount; i++) {
            pthread_mutex_destroy(&locks[i]);
        }
        pthread_mutex_destroy(&lbcache->lb_flock);
#endif
        lc_free(fs, lbcache->lb_pcacheLocks,
                sizeof(pthread_mutex_t) * lcount, LC_MEMTYPE_PCLOCK);
        lc_free(fs, lbcache, sizeof(struct lbcache), LC_MEMTYPE_LBCACHE);
    }
    fs->fs_bcache = NULL;
}

/* Find the lock hash for the list */
static inline uint32_t
lc_lockHash(struct fs *fs, uint64_t hash) {
    return hash % fs->fs_bcache->lb_pcacheLockCount;
}

/* Lock a hash list */
static inline uint32_t
lc_pcLockHash(struct fs *fs, uint64_t hash) {
    uint32_t lhash = lc_lockHash(fs, hash);

    pthread_mutex_lock(&fs->fs_bcache->lb_pcacheLocks[lhash]);
    return lhash;
}

/* Unlock a hash list */
static inline void
lc_pcUnLockHash(struct fs *fs, uint32_t lhash) {
    pthread_mutex_unlock(&fs->fs_bcache->lb_pcacheLocks[lhash]);
}

/* Return the read cluster block number */
static inline uint64_t
lc_clusterBlock(uint64_t block) {
    return block / LC_READ_CLUSTER_SIZE;
}

/* Lock taken while reading a page */
static inline uint32_t
lc_lockPageRead(struct fs *fs, uint64_t block) {
    uint32_t lhash = lc_lockHash(fs, lc_clusterBlock(block));

    pthread_mutex_lock(&fs->fs_bcache->lb_pioLocks[lhash]);
    return lhash;
}

/* Unlock a lock taken during page read */
static inline void
lc_unlockPageRead(struct fs *fs, uint32_t lhash) {
    pthread_mutex_unlock(&fs->fs_bcache->lb_pioLocks[lhash]);
}

/* Remove pages from page cache and free the hash table */
void
lc_destroyPages(struct gfs *gfs, struct fs *fs, bool remove) {
    struct page *page, **prev, *fpage = NULL;
    struct lbcache *lbcache = fs->fs_bcache;
    uint64_t i, count = 0, pcount;
    int gindex = fs->fs_pinval;
    struct pcache *pcache;
    uint32_t lhash = -1;
    bool all;

    if (lbcache == NULL) {
        return;
    }
    all = (fs->fs_parent == NULL);

    /* No need to process individual layers during an unmount */
    if (!all && (!remove || (gindex <= 0))) {
        fs->fs_bcache = NULL;
        return;
    }
    pcache = lbcache->lb_pcache;
    for (i = 0; i < lbcache->lb_pcacheSize; i++) {
        if (pcache[i].pc_head == NULL) {
            continue;
        }
        pcount = 0;
        if (!all) {
            if (fs->fs_rfs->fs_removed) {
                break;
            }
            lhash = lc_pcLockHash(fs, i);
            fpage = NULL;
        }
        page = pcache[i].pc_head;
        prev = &pcache[i].pc_head;
        while (page) {
            if (all || (page->p_lindex == gindex)) {
                *prev = page->p_cnext;
                page->p_block = LC_INVALID_BLOCK;
                page->p_dvalid = 0;
                pcount++;
                if (all) {
                    page->p_cnext = NULL;
                    lc_freePage(gfs, fs, page);
                } else {
                    page->p_cnext = fpage;
                    fpage = page;
                }
            } else {
                prev = &page->p_cnext;
            }
            page = *prev;
        }
        if (all) {
            assert(pcount == pcache[i].pc_pcount);
            assert(pcache[i].pc_head == NULL);
        } else {
            pcache[i].pc_pcount -= pcount;
            lc_pcUnLockHash(fs, lhash);

            /* Free the pages invalidated */
            while (fpage) {
                page = fpage;
                fpage = page->p_cnext;
                page->p_cnext = NULL;
                lc_freePage(gfs, fs, page);
            }
            if (fs->fs_rfs->fs_removed) {
                break;
            }
        }
        count += pcount;
    }

    /* Free the bcache header */
    lc_bcacheFree(fs);
    if (count && remove) {
        __sync_add_and_fetch(&gfs->gfs_preused, count);
    }
}

/* Remove a page from a hash list */
static void
lc_removePageFromHashList(struct pcache *pcache, struct page *page,
                          uint64_t hash) {
    assert(page->p_refCount == 0);
    assert(pcache[hash].pc_pcount > 0);

    page->p_block = LC_INVALID_BLOCK;
    page->p_cnext = NULL;
    pcache[hash].pc_pcount--;
}

/* Release a page */
void
lc_releasePage(struct gfs *gfs, struct fs *fs, struct page *page, bool read,
               bool inval) {
    bool invalidate = (inval || page->p_nocache) && !page->p_cache;
    struct pcache *pcache = fs->fs_bcache->lb_pcache;
    struct page *cpage, *fpage = NULL, **prev;
    uint32_t lhash;
    uint64_t hash;

    /* Move the page to the tail of the free list */
    if (!invalidate && (page->p_dnext == NULL) && !page->p_cache) {
        pthread_mutex_lock(&fs->fs_bcache->lb_flock);
        lc_removePageFromFreeList(fs->fs_bcache, page);
        lc_insertPageToFreeList(fs->fs_bcache, page);
        pthread_mutex_unlock(&fs->fs_bcache->lb_flock);
    }

    /* Find the hash list and lock it */
    hash = lc_pageBlockHash(fs, page->p_block);
    lhash = lc_pcLockHash(fs, hash);

    /* Decrement the reference count on the page */
    assert(page->p_refCount > 0);
    assert(!page->p_nohash);
    page->p_refCount--;

    /* If page does not have to be cached, then free it. */
    if (invalidate && (page->p_refCount == 0)) {
        cpage = pcache[hash].pc_head;
        prev = &pcache[hash].pc_head;

        /* Find the previous page in the singly linked list */
        while (cpage) {
            if (cpage == page) {
                *prev = page->p_cnext;
                break;
            }
            prev = &cpage->p_cnext;
            cpage = cpage->p_cnext;
        }
        assert(cpage);
        lc_removePageFromHashList(pcache, page, hash);
        fpage = page;
    } else if (read) {

        /* If page was read, increment hit count */
        page->p_hitCount++;
    }
    lc_pcUnLockHash(fs, lhash);

    /* Free the page picked for freeing */
    if (fpage) {
        lc_freePage(gfs, fs, fpage);
        __sync_add_and_fetch(&gfs->gfs_precycle, 1);
    }
}

/* Release a linked list of pages */
void
lc_releasePages(struct gfs *gfs, struct fs *fs, struct page *head,
                bool inval) {
    struct page *page = head, *next;
    uint64_t count = 0;

    while (page) {
        next = page->p_dnext;
        page->p_dnext = NULL;

        /* If block is not in the cache, free it */
        if ((page->p_block == LC_INVALID_BLOCK) || page->p_nohash) {
            assert(page->p_refCount == 1);
            page->p_block = LC_INVALID_BLOCK;
            page->p_refCount = 0;
            lc_freePage(gfs, fs, page);
            count++;
        } else if (inval && fs->fs_removed) {
            assert(page->p_refCount == 1);
            page->p_refCount = 0;
        } else {
            lc_releasePage(gfs, fs, page, false, inval);
        }
        page = next;
    }
    if (count) {
        __sync_add_and_fetch(&gfs->gfs_precycle, count);
    }
}

/* Release pages */
void
lc_releaseReadPages(struct gfs *gfs, struct fs *fs,
                    struct page **pages, uint64_t pcount, bool nocache) {
    uint64_t i;

    for (i = 0; i < pcount; i++) {
        lc_releasePage(gfs, fs, pages[i], true, nocache);
    }
}

/* Invalidate a page if present in cache */
int
lc_invalPage(struct gfs *gfs, struct fs *fs, uint64_t block) {
    struct pcache *pcache = fs->fs_bcache->lb_pcache;
    int hash = lc_pageBlockHash(fs, block);
    struct page *page = NULL, **prev = &pcache[hash].pc_head;
    uint32_t lhash, ret = 0;

    if (pcache[hash].pc_head == NULL) {
        return 0;
    }
    lhash = lc_pcLockHash(fs, hash);
    page = pcache[hash].pc_head;

    /* Traverse the list looking for the page and invalidate it if found */
    while (page) {
        if (page->p_block == block) {
            page->p_cache = 0;
            if (page->p_refCount) {
                page->p_nocache = 1;
                page = NULL;
                break;
            }
            *prev = page->p_cnext;
            lc_removePageFromHashList(pcache, page, hash);
            break;
        }
        prev = &page->p_cnext;
        page = page->p_cnext;
    }
    lc_pcUnLockHash(fs, lhash);

    /* Free the page */
    if (page) {
        lc_freePage(gfs, fs, page);
        ret = 1;
    }
    return ret;
}

/* Set block number on a page */
void
lc_setPageBlock(struct page *page, uint64_t block) {
    assert(page->p_block == LC_INVALID_BLOCK);
    assert(page->p_refCount == 1);
    assert(page->p_cnext == NULL);
    page->p_block = block;
    page->p_nohash = 1;
}

/* Add a page to page block hash list */
void
lc_addPageBlockHash(struct gfs *gfs, struct fs *fs,
                    struct page *page, uint64_t block) {
    struct pcache *pcache = fs->fs_bcache->lb_pcache;
    int hash = lc_pageBlockHash(fs, block);
    struct page *cpage, **prev;
    uint32_t lhash;

    /* Initialize the page structure and lock the hash list */
    lc_setPageBlock(page, block);
    lhash = lc_pcLockHash(fs, hash);
    cpage = pcache[hash].pc_head;
    prev = &pcache[hash].pc_head;

    /* Invalidate previous instance of this block if there is one.
     * Blocks are not invalidated in cache when freed.
     */
    while (cpage) {
        if (cpage->p_block == block) {
            *prev = cpage->p_cnext;
            lc_removePageFromHashList(pcache, cpage, hash);
            break;
        }
        prev = &cpage->p_cnext;
        cpage = cpage->p_cnext;
    }

    /* Add the new page at the head of the list */
    page->p_cnext = pcache[hash].pc_head;
    pcache[hash].pc_head = page;
    pcache[hash].pc_pcount++;
    lc_pcUnLockHash(fs, lhash);
    if (cpage) {
        lc_freePage(gfs, fs, cpage);
    }
}

/* Lookup/Create a page in the block hash */
struct page *
lc_getPage(struct fs *fs, uint64_t block, char *data, bool read) {
    int hash = lc_pageBlockHash(fs, block), gindex = fs->fs_gindex;
    struct pcache *pcache = fs->fs_bcache->lb_pcache;
    bool hit = false, missed = false;
    struct page *page, *new = NULL;
    struct gfs *gfs = fs->fs_gfs;
    uint32_t lhash;

    /* Lock the hash list and look for a page */

retry:
    lhash = lc_pcLockHash(fs, hash);
    page = pcache[hash].pc_head;
    while (page && (page->p_block != block)) {
        page = page->p_cnext;
    }
    hit = (page != NULL);
    if (hit) {

        /* If a page is found, increment reference count */
        page->p_refCount++;
        if (page->p_lindex != gindex) {

            /* If a page is shared by many layers, untag it */
            page->p_lindex = 0;
        }
    } else if (new) {

        /* If page is not found, instantiate one */
        page = new;
        new = NULL;
        page->p_block = block;
        page->p_cnext = pcache[hash].pc_head;
        pcache[hash].pc_head = page;
        pcache[hash].pc_pcount++;
    }
    lc_pcUnLockHash(fs, lhash);

    /* If no page is found, allocate one and retry */
    if (page == NULL) {
        new = lc_newPage(gfs, fs);
        assert(!new->p_dvalid);
        new->p_lindex = gindex;
        if ((fs->fs_pinval != -1) && !fs->fs_readOnly &&
            !(fs->fs_super->sb_flags & LC_SUPER_INIT)) {
            fs->fs_pinval = gindex;
        }
        goto retry;
    }

    /* If raced with another thread, free the unused page */
    if (new) {
        new->p_refCount = 0;
        lc_freePage(gfs, fs, new);
    }

    /* If page is missing data, read from disk */
    if (read && !page->p_dvalid) {
        lhash = lc_lockPageRead(fs, block);
        if (!page->p_dvalid) {
            if (data) {
                page->p_data = data;
            } else {
                if (page->p_data == NULL) {
                    lc_mallocBlockAligned(fs->fs_rfs, (void **)&page->p_data,
                                          LC_MEMTYPE_DATA);
                }
                lc_readBlock(gfs, fs, block, page->p_data);
                page->p_dvalid = 1;
                missed = true;
            }
        }
        lc_unlockPageRead(fs, lhash);
    }
    assert(page->p_refCount > 0);
    assert(!read || page->p_data);
    assert(!read || page->p_dvalid);
    assert(page->p_block == block);
    if (missed) {
        __sync_add_and_fetch(&gfs->gfs_pmissed, 1);
    } else if (hit) {
        __sync_add_and_fetch(&gfs->gfs_phit, 1);
    }
    return page;
}

/* Link new data to the page of a file */
struct page *
lc_getPageNew(struct gfs *gfs, struct fs *fs, uint64_t block, char *data) {
    struct page *page = lc_getPage(fs, block, NULL, false);

    assert(page->p_refCount == 1);

    /* If page already has data associated with, free that */
    if (page->p_data) {
        lc_freePageData(gfs, fs->fs_rfs, page->p_data);
    }
    page->p_data = data;
    page->p_dvalid = 1;
    page->p_hitCount = 0;
    return page;
}

/* Get a page with no block associated with.  Blocks will be allocated later
 * and the page will be added to hash then.  This interface for allocating
 * space contiguously for a set of pages.
 */
struct page *
lc_getPageNoBlock(struct gfs *gfs, struct fs *fs, char *data,
                  struct page *prev) {
    struct page *page = lc_newPage(gfs, fs);

    page->p_data = data;
    page->p_dvalid = 1;
    page->p_dnext = prev;
    return page;
}

/* Get a page for the block without reading it from disk, but making sure a
 * data buffer exists for copying in data.  New data will be copied to the
 * page by the caller.
 */
struct page *
lc_getPageNewData(struct fs *fs, uint64_t block, char *data) {
    struct page *page;

    page = lc_getPage(fs, block, data, data);
    if ((page->p_data == NULL) && (data == NULL)) {
        lc_mallocBlockAligned(fs->fs_rfs, (void **)&page->p_data,
                              LC_MEMTYPE_DATA);
    }
    page->p_hitCount = 0;
    return page;
}

/* Read in a cluster of blocks */
void
lc_readPages(struct gfs *gfs, struct fs *fs, struct page **pages,
             uint32_t count) {
    uint32_t i, iovcnt = 0, j = 0, rcount = 0;
    uint64_t sblock, pblock = 0, cblock;
    struct page *page = pages[0];
    struct iovec *iovec;
    uint32_t lhash;

    /* Use pread(2) interface if there is just one block to read */
    if (count == 1) {

        /* Check if the page has valid data after racing with another thread */
        if (!page->p_dvalid) {
            sblock = page->p_block;
            lhash = lc_lockPageRead(fs, sblock);
            if (!page->p_dvalid) {
                lc_readBlock(gfs, fs, sblock, page->p_data);
                page->p_dvalid = 1;
                rcount = 1;
            }
            lc_unlockPageRead(fs, lhash);
        }
    } else {
        iovec = alloca(count * sizeof(struct iovec));
        sblock = page->p_block;
        cblock = lc_clusterBlock(sblock);
        lhash = lc_lockPageRead(fs, sblock);
        for (i = 0; i < count; i++) {
            page = pages[i];

            /* Skip pages with valid data (raced with another thread) */
            if (page->p_dvalid) {
                continue;
            }

            /* Issue if pages are not contiguous on disk, spanning across
             * read clusters or iov accumulated maximum allowed.
             */
            if (iovcnt &&
                (((pblock + 1) != page->p_block) ||
                (cblock != lc_clusterBlock(page->p_block)) ||
                (iovcnt >= LC_READ_CLUSTER_SIZE))) {
                lc_readBlocks(gfs, fs, iovec, iovcnt, sblock);

                /* Mark pages having valid data */
                for (; j < i; j++) {
                    pages[j]->p_dvalid = 1;
                }
                rcount += iovcnt;
                iovcnt = 0;
            }

            /* When a new iovec is started, get the right lock */
            if (i && (iovcnt == 0)) {
                sblock = page->p_block;
                if (cblock != lc_clusterBlock(sblock)) {
                    lc_unlockPageRead(fs, lhash);
                    lhash = lc_lockPageRead(fs, sblock);
                    if (page->p_dvalid) {
                        continue;
                    }
                }
                cblock = lc_clusterBlock(sblock);
            }

            /* Add the page to iovec */
            assert((page->p_block == sblock) ||
                   (page->p_block == (pblock + 1)));
            pblock = page->p_block;
            assert(cblock == lc_clusterBlock(pblock));
            iovec[iovcnt].iov_base = page->p_data;
            iovec[iovcnt].iov_len = LC_BLOCK_SIZE;
            iovcnt++;
        }

        /* Issue I/O on any remaining pages */
        if (iovcnt) {
            lc_readBlocks(gfs, fs, iovec, iovcnt, sblock);
            for (; j < count; j++) {
                pages[j]->p_dvalid = 1;
            }
            rcount += iovcnt;
        }
        lc_unlockPageRead(fs, lhash);
    }
    if (rcount) {

        /* Consider all the pages read as missed in the cache */
        __sync_add_and_fetch(&gfs->gfs_pmissed, rcount);
    }
}

/* Flush a cluster of pages */
static void
lc_flushPageCluster(struct gfs *gfs, struct fs *fs,
                    struct page *head, uint64_t count) {
    struct page *page = head;
    uint64_t i, j, iovcount;
    struct iovec *iovec;
    uint64_t block = 0;

    /* Mark superblock dirty before modifying something */
    lc_markSuperDirty(fs);

    /* Use pwrite(2) interface if there is just one block */
    if (count == 1) {
        block = page->p_block;
        assert(block != 0);
        lc_writeBlock(gfs, fs, page->p_data, block);
    } else {
        iovcount = (count < LC_WRITE_CLUSTER_SIZE) ?
                        count : LC_WRITE_CLUSTER_SIZE;
        iovec = alloca(iovcount * sizeof(struct iovec));

        /* Issue the I/O in block order */
        for (i = 0, j = 0; i < count; i++, j++) {

            /* Flush current set of dirty pages if the new page is not adjacent
             * to those.
             * XXX This could happen when metadata and userdata are flushed
             * concurrently OR files flushed concurrently.
             */
            if ((j >= iovcount) || (j && ((block + j) != page->p_block))) {
                assert(block != 0);
                if (j == 1) {
                    lc_writeBlock(gfs, fs, iovec[0].iov_base, block);
                } else {
                    lc_writeBlocks(gfs, fs, iovec, j, block);
                }
                j = 0;
            }
            iovec[j].iov_base = page->p_data;
            iovec[j].iov_len = LC_BLOCK_SIZE;
            if (j == 0) {
                block = page->p_block;
            }
            page = page->p_dnext;
        }
        assert(page == NULL);
        assert(block != 0);
        if (j == 1) {
            lc_writeBlock(gfs, fs, iovec[0].iov_base, block);
        } else {
            lc_writeBlocks(gfs, fs, iovec, j, block);
        }
    }

    /* Release the pages after writing */
    lc_releasePages(gfs, fs, head, fs->fs_removed);
}

/* Add a page to the file system dirty list for writeback */
void
lc_addPageForWriteBack(struct gfs *gfs, struct fs *fs, struct page *head,
                       struct page *tail, uint64_t pcount) {
    assert(tail->p_dnext == NULL);
    pthread_mutex_lock(&fs->fs_plock);
    if (fs->fs_dpages == NULL) {
        fs->fs_dpages = head;
    } else {
        fs->fs_dpagesLast->p_dnext = head;
    }
    fs->fs_dpagesLast = tail;
    fs->fs_dpcount += pcount;
    pthread_mutex_unlock(&fs->fs_plock);

    /* Signal syncer has work to do */
    if (!fs->fs_readOnly && (fs->fs_dpcount > LC_SYNCER_DIRTY_COUNT)) {
        lc_layerChanged(gfs, false, false);
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
        fs->fs_dpagesLast = NULL;
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
        fs->fs_dpagesLast = NULL;
        fs->fs_dpcount = 0;
        pthread_mutex_unlock(&fs->fs_plock);
        lc_releasePages(gfs, fs, page, true);
    }
}

/* Background thread for flushing dirty pages */
void *
lc_flusher(void *data) {
    struct gfs *gfs = (struct gfs *)data;
    struct timespec interval;
    struct timeval now;
    time_t recent = 0;
    struct fs *fs;
    bool force;
    int i;

    interval.tv_nsec = 0;
    while (!gfs->gfs_unmounting) {
        gettimeofday(&now, NULL);
        interval.tv_sec = now.tv_sec + LC_FLUSH_INTERVAL;
        pthread_mutex_lock(&gfs->gfs_flock);
        pthread_cond_timedwait(&gfs->gfs_flusherCond, &gfs->gfs_flock,
                               &interval);
        pthread_mutex_unlock(&gfs->gfs_flock);
        rcu_register_thread();
        rcu_read_lock();

        /* Check if any layers accumulated too many dirty pages */
        for (i = 0; i <= gfs->gfs_scount; i++) {
            fs = rcu_dereference(gfs->gfs_fs[i]);
            if (fs == NULL) {
                continue;
            }
            force = !lc_checkMemoryAvailable(true) ||
                    gfs->gfs_pcleaning || gfs->gfs_pcleaningForced;

            /* Skip newly created layers */
            gettimeofday(&now, NULL);
            recent = now.tv_sec - LC_FLUSH_TIME;

            /* Flush dirty data pages from read-write layers.
             * Dirty data from read only layers are flushed as those are
             * created.
             */
            if (!fs->fs_readOnly && fs->fs_pcount &&
                ((fs->fs_pcount >= LC_MAX_LAYER_DIRTYPAGES) ||
                 force || (fs->fs_super->sb_ctime < recent)) &&
                !(fs->fs_super->sb_flags & LC_SUPER_INIT) &&
                !lc_tryLock(fs, false)) {
                rcu_read_unlock();
                if (fs->fs_pcount) {
                    lc_flushDirtyInodeList(fs, force);
                }
                lc_flushDirtyPages(gfs, fs);
                lc_unlock(fs);
                rcu_read_lock();
            } else if (((fs->fs_dpcount >= LC_SYNCER_DIRTY_COUNT) ||
                        (fs->fs_dpcount && force)) &&
                       !lc_tryLock(fs, false)) {
                rcu_read_unlock();

                /* Write out dirty pages of a layer */
                lc_flushDirtyPages(gfs, fs);
                lc_unlock(fs);
                rcu_read_lock();
            }
        }
        rcu_read_unlock();
        rcu_unregister_thread();
    }
    return NULL;
}

/* Wakeup cleaner thread and wait for it to free up memory */
void
lc_wakeupCleaner(struct gfs *gfs, bool wait) {

    if (!wait) {

        /* If no need to wait, just wake up cleaner and return */
        if (!gfs->gfs_pcleaning) {
            pthread_cond_signal(&gfs->gfs_flusherCond);
            pthread_cond_signal(&gfs->gfs_cleanerCond);
        }
        return;
    }

    /* Wakeup cleaner and wait to be woken up */
    pthread_mutex_lock(&gfs->gfs_clock);
    if (!gfs->gfs_pcleaning) {
        pthread_cond_signal(&gfs->gfs_flusherCond);

        /* Let a single thread do the job to avoid contention on locks */
        gfs->gfs_pcleaning = true;
        pthread_cond_signal(&gfs->gfs_cleanerCond);
    }
    while (!lc_checkMemoryAvailable(false) && gfs->gfs_pcleaning) {
        pthread_cond_wait(&gfs->gfs_mcond, &gfs->gfs_clock);
    }
    pthread_mutex_unlock(&gfs->gfs_clock);
}

/* Purge some pages of a tree of layers */
static uint64_t
lc_purgeTreePages(struct gfs *gfs, struct fs *fs, uint64_t *blocks,
                  bool force) {
    struct lbcache *lbcache = fs->fs_bcache;
    bool all = gfs->gfs_pcleaningForced;
    uint64_t count = 0, pcount = 0;
    struct page *page;

    assert(fs->fs_parent == NULL);

    if (lbcache->lb_fhead == NULL) {
        return 0;
    }

    /* Invalidate pages from the head of the free list */
    pthread_mutex_lock(&lbcache->lb_flock);
    page = lbcache->lb_fhead;
    while (page && (pcount < LC_PAGE_PURGE_COUNT)) {
        if ((page->p_block != LC_INVALID_BLOCK) &&
            (all || (page->p_refCount == 0))) {
            if (!all && page->p_hitCount) {
                page->p_hitCount--;
            } else {
                blocks[pcount++] = page->p_block;
            }
        }
        page = page->p_fnext;
    }
    pthread_mutex_unlock(&lbcache->lb_flock);
    while (pcount && !fs->fs_removed) {
        count += lc_invalPage(gfs, fs, blocks[--pcount]);
    }
    return count;
}

/* Free pages when running low on memory */
static void
lc_purgePages(struct gfs *gfs, bool force) {
    uint64_t blocks[LC_PAGE_PURGE_COUNT], count = 0, tcount, pcount = 0;
    struct fs *fs;
    int i;

    gfs->gfs_pcleaning = true;
    rcu_register_thread();

retry:
    rcu_read_lock();
    for (i = 0; i <= gfs->gfs_scount; i++) {

        /* Start from a file system after the one processed last time */
        if (gfs->gfs_cleanerIndex > gfs->gfs_scount) {
            gfs->gfs_cleanerIndex = 0;
        }
        fs = rcu_dereference(gfs->gfs_fs[gfs->gfs_cleanerIndex++]);
        if ((fs == NULL) || fs->fs_parent || lc_tryLock(fs, false)) {
            continue;
        }

        /* Purge clean pages for the tree */
        rcu_read_unlock();
        do {
            tcount = lc_purgeTreePages(gfs, fs, blocks, force);
            pcount += tcount;
        } while (tcount && gfs->gfs_pcleaningForced);
        lc_unlock(fs);
        rcu_read_lock();
        if (!gfs->gfs_pcleaningForced && lc_checkMemoryAvailable(true)) {
            break;
        }
        if (tcount && lc_checkMemoryAvailable(false)) {
            pthread_cond_broadcast(&gfs->gfs_mcond);
        }
    }
    rcu_read_unlock();
    count += pcount;

    /* Wakeup flusher */
    if (!lc_checkMemoryAvailable(true) && !gfs->gfs_unmounting) {
        pthread_cond_signal(&gfs->gfs_flusherCond);
        if (pcount) {
            pcount = 0;
            goto retry;
        }
    }
    gfs->gfs_pcleaning = false;
    gfs->gfs_pcleaningForced = false;

    /* Wakeup threads waiting for memory to become available */
    pthread_mutex_lock(&gfs->gfs_clock);
    pthread_cond_broadcast(&gfs->gfs_mcond);
    pthread_mutex_unlock(&gfs->gfs_clock);
    rcu_unregister_thread();
    if (count) {
        gfs->gfs_purged += count;
    }
}

/* Background thread for purging clean pages */
void
lc_cleaner(void) {
    struct gfs *gfs = getfs();
    struct timespec interval;
    struct timeval now;

    /* Purge clean pages when amount of memory used for pages goes above a
     * certain threshold.
     */
    interval.tv_nsec = 0;
    while (!gfs->gfs_unmounting) {
        gettimeofday(&now, NULL);
        interval.tv_sec = now.tv_sec + LC_CLEAN_INTERVAL;
        pthread_mutex_lock(&gfs->gfs_clock);
        if (!gfs->gfs_pcleaning) {
            pthread_cond_timedwait(&gfs->gfs_cleanerCond,
                                   &gfs->gfs_clock, &interval);
        }
        pthread_mutex_unlock(&gfs->gfs_clock);
        if (!gfs->gfs_unmounting) {
            lc_purgePages(gfs, !lc_checkMemoryAvailable(true));
        }
    }
}
