#include "includes.h"

/* Return the hash number for the block number provided */
/* XXX Figure out a better hashing scheme */
static inline int
lc_pageBlockHash(struct fs *fs, uint64_t block) {
    assert(block);
    assert(block != LC_INVALID_BLOCK);
    return block % fs->fs_bcache->lb_pcacheSize;
}

/* Allocate a new page. Memory is counted against the base layer */
static struct page *
lc_newPage(struct gfs *gfs, struct fs *fs) {
    struct page *page = lc_malloc(fs->fs_rfs, sizeof(struct page),
                                  LC_MEMTYPE_PAGE);

    page->p_data = NULL;
    page->p_block = LC_INVALID_BLOCK;
    page->p_refCount = 1;
    page->p_hitCount = 0;
    page->p_cnext = NULL;
    page->p_dnext = NULL;
    page->p_dvalid = 0;
    __sync_add_and_fetch(&fs->fs_bcache->lb_pcount, 1);
    __sync_add_and_fetch(&gfs->gfs_pcount, 1);
    return page;
}

/* Free a page */
static void
lc_freePage(struct gfs *gfs, struct fs *fs, struct page *page) {
    assert(page->p_refCount == 0);
    assert(page->p_block == LC_INVALID_BLOCK);
    assert(page->p_cnext == NULL);
    assert(page->p_dnext == NULL);

    if (page->p_data) {
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
    lbcache->lb_pcacheSize = count;
    lbcache->lb_pcacheLockCount = lcount;
    lbcache->lb_pcount = 0;
    fs->fs_bcache = lbcache;
}

/* Free bcache structure */
void
lc_bcacheFree(struct fs *fs) {
    struct lbcache *lbcache = fs->fs_bcache;
    pthread_mutex_t *locks;
    uint32_t i, lcount;

    /* Free the bcache when the base layer is deleted/unmounted */
    if (fs->fs_parent == NULL) {
        assert(lbcache->lb_pcount == 0);
        lc_free(fs, lbcache->lb_pcache,
                sizeof(struct pcache) * lbcache->lb_pcacheSize,
                LC_MEMTYPE_PCACHE);
        lcount = lbcache->lb_pcacheLockCount * 2;
        locks = lbcache->lb_pcacheLocks;
        for (i = 0; i < lcount; i++) {
            pthread_mutex_destroy(&locks[i]);
        }
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

/* Lock taken while reading a page */
static inline uint32_t
lc_lockPageRead(struct fs *fs, uint64_t block) {
    uint32_t lhash = lc_lockHash(fs, block);

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
    uint64_t i, count = 0, pcount, gindex = fs->fs_gindex;
    struct lbcache *lbcache = fs->fs_bcache;
    struct page *page, **prev;
    struct pcache *pcache;
    uint32_t lhash;
    bool all;

    if (lbcache == NULL) {
        return;
    }
    all = (fs->fs_parent == NULL);

    /* No need to process individual layers during an unmount */
    /* XXX Disabled for now as this is slowing down layer removals */
    if (!all && (!remove || true)) {
        fs->fs_bcache = NULL;
        return;
    }
    pcache = lbcache->lb_pcache;
    for (i = 0; i < lbcache->lb_pcacheSize; i++) {
        pcount = 0;
        lhash = lc_pcLockHash(fs, i);
        page = pcache[i].pc_head;
        prev = &pcache[i].pc_head;
        while (page) {
            if (all || (page->p_lindex == gindex)) {
                *prev = page->p_cnext;
                page->p_block = LC_INVALID_BLOCK;
                page->p_cnext = NULL;
                page->p_dvalid = 0;
                lc_freePage(gfs, fs, page);
                pcount++;
            } else {
                prev = &page->p_cnext;
            }
            page = *prev;
        }
        if (all) {
            assert(pcount == pcache[i].pc_pcount);
            assert(pcache[i].pc_head == NULL);
        }
        lc_pcUnLockHash(fs, lhash);
        count += pcount;
    }

    /* Free the bcache header */
    lc_bcacheFree(fs);
    if (count && remove) {
        __sync_add_and_fetch(&gfs->gfs_preused, count);
    }
}

/* Release a page */
void
lc_releasePage(struct gfs *gfs, struct fs *fs, struct page *page, bool read) {
    struct page *cpage, *prev = NULL, *fpage = NULL, **fprev;
    struct pcache *pcache = fs->fs_bcache->lb_pcache;
    uint64_t hash, hit, count = 0;
    uint32_t lhash;

    /* Find the hash list and lock it */
    hash = lc_pageBlockHash(fs, page->p_block);
    lhash = lc_pcLockHash(fs, hash);

    /* Decrement the reference count on the page */
    assert(page->p_refCount > 0);
    page->p_refCount--;

    /* If page does not have to be cached, then free it. */
    fprev = &pcache[hash].pc_head;
    if (page->p_nocache && (page->p_refCount == 0)) {

        /* Find the previous page in the singly linked list */
        if (pcache[hash].pc_head != page) {
            cpage = pcache[hash].pc_head;
            while (cpage) {
                if (cpage->p_cnext == page) {
                    fprev = &cpage->p_cnext;
                    break;
                }
                cpage = cpage->p_cnext;
            }
            assert(fprev != &pcache[hash].pc_head);
        }
        fpage = page;
    } else if (read) {

        /* If page was read, increment hit count */
        page->p_hitCount++;
    }

    /* Free a page if this hash list accumulated more than a certain number of
     * pages.  This is a cheap LRU scheme for better performance compared to a
     * global LRU scheme.
     */
    if ((fpage == NULL) &&
        (pcache[hash].pc_pcount >
         (LC_PAGE_MAX / fs->fs_bcache->lb_pcacheSize))) {
        cpage = pcache[hash].pc_head;
        hit = page->p_hitCount;

        /* Look for a page with lowest hit count */
        while (cpage) {
            if ((cpage->p_refCount == 0) && (cpage->p_hitCount <= hit)) {
                if (prev) {
                    fprev = &prev->p_cnext;
                } else {
                    assert(fprev == &pcache[hash].pc_head);
                }
                fpage = cpage;
                hit = cpage->p_hitCount;
            }

            /* Stop linear search after a certain number of pages */
            if ((hit == 0) && fpage && (count > LC_CACHE_PURGE_CHECK_MAX)) {
                break;
            }
            prev = cpage;
            cpage = cpage->p_cnext;
            count++;
        }
    }

    /* If a page is picked for freeing, take off page from block cache */
    if (fpage) {
        *fprev = fpage->p_cnext;
        fpage->p_block = LC_INVALID_BLOCK;
        fpage->p_cnext = NULL;
        assert(pcache[hash].pc_pcount > 0);
        pcache[hash].pc_pcount--;
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
lc_releasePages(struct gfs *gfs, struct fs *fs, struct page *head) {
    struct page *page = head, *next;

    while (page) {
        next = page->p_dnext;
        page->p_dnext = NULL;

        /* If block is not in the cache, free it */
        if (page->p_block == LC_INVALID_BLOCK) {
            assert(page->p_refCount == 1);
            page->p_refCount = 0;
            lc_freePage(gfs, fs, page);
        } else {
            lc_releasePage(gfs, fs, page, false);
        }
        page = next;
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

/* Invalidate a page if present in cache */
int
lc_invalPage(struct gfs *gfs, struct fs *fs, uint64_t block) {
    struct pcache *pcache = fs->fs_bcache->lb_pcache;
    int hash = lc_pageBlockHash(fs, block);
    struct page *page = NULL, **prev = &pcache[hash].pc_head;
    uint32_t lhash;

    if (pcache[hash].pc_head == NULL) {
        return 0;
    }
    lhash = lc_pcLockHash(fs, hash);
    page = pcache[hash].pc_head;

    /* Traverse the list looking for the page and invalidate it if found */
    while (page) {
        if (page->p_block == block) {
            assert(page->p_refCount == 0);
            *prev = page->p_cnext;
            page->p_cnext = NULL;
            page->p_block = LC_INVALID_BLOCK;
            assert(pcache[hash].pc_pcount > 0);
            pcache[hash].pc_pcount--;
            break;
        }
        prev = &page->p_cnext;
        page = page->p_cnext;
    }
    lc_pcUnLockHash(fs, lhash);

    /* Free the page */
    if (page) {
        lc_freePage(gfs, fs, page);
        return 1;
    }
    return 0;
}

/* Add a page to page block hash list */
void
lc_addPageBlockHash(struct gfs *gfs, struct fs *fs,
                    struct page *page, uint64_t block) {
    struct pcache *pcache = fs->fs_bcache->lb_pcache;
    int hash = lc_pageBlockHash(fs, block);
    struct page *cpage;
    uint32_t lhash;

    /* Initialize the page structure and lock the hash list */
    assert(page->p_block == LC_INVALID_BLOCK);
    page->p_block = block;
    page->p_lindex = fs->fs_gindex;
    page->p_nocache = true;
    lhash = lc_pcLockHash(fs, hash);
    cpage = pcache[hash].pc_head;

    /* Invalidate previous instance of this block if there is one.
     * Blocks are not invalidated in cache when freed.
     */
    while (cpage) {
        if (cpage->p_block == block) {
            assert(cpage->p_refCount == 0);
            cpage->p_block = LC_INVALID_BLOCK;
            break;
        }
        cpage = cpage->p_cnext;
    }

    /* Add the new page at the head of the list */
    page->p_cnext = pcache[hash].pc_head;
    pcache[hash].pc_head = page;
    pcache[hash].pc_pcount++;
    lc_pcUnLockHash(fs, lhash);
}

/* Lookup/Create a page in the block hash */
struct page *
lc_getPage(struct fs *fs, uint64_t block, bool read) {
    int hash = lc_pageBlockHash(fs, block);
    struct pcache *pcache = fs->fs_bcache->lb_pcache;
    struct page *page, *new = NULL;
    struct gfs *gfs = fs->fs_gfs;
    bool hit = false;
    uint32_t lhash;

    assert(block);
    assert(block != LC_PAGE_HOLE);

    /* Lock the hash list and look for a page */

retry:
    lhash = lc_pcLockHash(fs, hash);
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
        page->p_lindex = fs->fs_gindex;
        page->p_cnext = pcache[hash].pc_head;
        pcache[hash].pc_head = page;
        pcache[hash].pc_pcount++;
    }
    lc_pcUnLockHash(fs, lhash);

    /* If no page is found, allocate one and retry */
    if (page == NULL) {
        new = lc_newPage(gfs, fs);
        assert(!new->p_dvalid);
        goto retry;
    } else if (page->p_lindex != fs->fs_gindex) {

        /* If a page is shared by many layers, untag it */
        page->p_lindex = 0;
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
            lc_mallocBlockAligned(fs->fs_rfs, (void **)&page->p_data,
                                  LC_MEMTYPE_DATA);
            lc_readBlock(gfs, fs, block, page->p_data);
            page->p_dvalid = 1;
        }
        lc_unlockPageRead(fs, lhash);
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

/* Link new data to the page of a file */
struct page *
lc_getPageNew(struct gfs *gfs, struct fs *fs, uint64_t block, char *data) {
    struct page *page = lc_getPage(fs, block, false);

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
lc_getPageNewData(struct fs *fs, uint64_t block) {
    struct page *page;

    page = lc_getPage(fs, block, false);
    if (page->p_data == NULL) {
        lc_mallocBlockAligned(fs->fs_rfs, (void **)&page->p_data,
                              LC_MEMTYPE_DATA);
    }
    page->p_hitCount = 0;
    return page;
}

/* Flush a cluster of pages */
void
lc_flushPageCluster(struct gfs *gfs, struct fs *fs,
                    struct page *head, uint64_t count, bool bfree) {
    struct extent *extents = NULL;
    uint64_t i, j, bcount = 0;
    struct page *page = head;
    struct iovec *iovec;
    uint64_t block = 0;

    /* Use pwrite(2) interface if there is just one block */
    if (count == 1) {
        block = page->p_block;
        lc_writeBlock(gfs, fs, page->p_data, block);
    } else {
        iovec = alloca(count * sizeof(struct iovec));

        /* Issue the I/O in block order */
        for (i = 0, j = count - 1; i < count; i++, j--) {

            /* Flush current set of dirty pages if the new page is not adjacent
             * to those.
             * XXX This could happen when metadata and userdata are flushed
             * concurrently OR files flushed concurrently.
             */
            if ((i && ((page->p_block + 1) != block)) ||
                (bcount >= LC_CLUSTER_SIZE)) {
                //lc_printf("Not contigous, block %ld previous block %ld i %ld count %ld\n", block, page->p_block, i, count);
                lc_writeBlocks(gfs, fs, &iovec[j + 1], bcount, block);
                bcount = 0;
            }
            iovec[j].iov_base = page->p_data;
            iovec[j].iov_len = LC_BLOCK_SIZE;
            block = page->p_block;
            bcount++;
            page = page->p_dnext;
        }
        assert(page == NULL);
        assert(block != 0);
        lc_writeBlocks(gfs, fs, iovec, bcount, block);
    }

    /* Release the pages after writing */
    lc_releasePages(gfs, fs, head);

    /* Check any of the freed blocks can be released to the free pool */
    if (bfree) {
        pthread_mutex_lock(&fs->fs_plock);
        assert(fs->fs_wpcount >= count);
        fs->fs_wpcount -= count;

        /* If no writes are pending and none in progress, then release the
         * freed blocks for reuse.
         */
        if ((fs->fs_wpcount == 0) && (fs->fs_dpcount == 0) &&
            fs->fs_fdextents) {
            pthread_mutex_lock(&fs->fs_alock);
            extents = fs->fs_fdextents;
            fs->fs_fdextents = NULL;
            pthread_mutex_unlock(&fs->fs_alock);
        }
        pthread_mutex_unlock(&fs->fs_plock);
        if (extents) {
            lc_blockFreeExtents(fs, extents,
                                fs->fs_removed ? 0 :
                                (LC_EXTENT_EFREE | LC_EXTENT_LAYER));
        }
    }
}

/* Add a page to the file system dirty list for writeback */
void
lc_addPageForWriteBack(struct gfs *gfs, struct fs *fs, struct page *head,
                       struct page *tail, uint64_t pcount) {
    struct page *page = NULL;
    uint64_t count = 0;

    pthread_mutex_lock(&fs->fs_plock);
    tail->p_dnext = fs->fs_dpages;
    fs->fs_dpages = head;
    fs->fs_dpcount += pcount;

    /* Issue write when a certain number of dirty pages accumulated */
    if (fs->fs_dpcount >= LC_CLUSTER_SIZE) {
        page = fs->fs_dpages;
        fs->fs_dpages = NULL;
        count = fs->fs_dpcount;
        fs->fs_dpcount = 0;
        fs->fs_wpcount += count;
    }
    pthread_mutex_unlock(&fs->fs_plock);
    if (count) {
        lc_flushPageCluster(gfs, fs, page, count, true);
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
        fs->fs_wpcount += count;
        pthread_mutex_unlock(&fs->fs_plock);
        if (count) {
            lc_flushPageCluster(gfs, fs, page, count, true);
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

/* Purge some pages of a tree of layers */
static uint64_t
lc_purgeTreePages(struct gfs *gfs, struct fs *fs, bool force) {
    struct lbcache *lbcache = fs->fs_bcache;
    struct pcache *pcache = lbcache->lb_pcache;
    struct page *page, **prev;
    uint64_t i, j, count = 0;
    uint32_t lhash;

    assert(fs->fs_parent == NULL);
    for (j = 0; j < lbcache->lb_pcacheSize; j++) {
        if (lbcache->lb_pcount == 0) {
            break;
        }

        /* Start from the hashlist where processing stopped previously */
        i = fs->fs_purgeIndex++;
        if (fs->fs_purgeIndex >= lbcache->lb_pcacheSize) {
             fs->fs_purgeIndex = 0;
        }
        assert(i < lbcache->lb_pcacheSize);
        if (pcache[i].pc_pcount == 0) {
            continue;
        }
        lhash = lc_pcLockHash(fs, i);
        prev = &pcache[i].pc_head;
        page = pcache[i].pc_head;
        while (page) {

            /* Free pages if not in use currently */
            if (page->p_refCount == 0) {
                *prev = page->p_cnext;
                page->p_cnext = NULL;
                page->p_block = LC_INVALID_BLOCK;
                page->p_dvalid = 0;
                lc_freePage(gfs, fs, page);
                assert(pcache[i].pc_pcount > 0);
                pcache[i].pc_pcount--;
                count++;
                page = *prev;
                continue;
            }
            prev = &page->p_cnext;
            page = page->p_cnext;
        }
        lc_pcUnLockHash(fs, lhash);
        if (!force && (gfs->gfs_pcount < (LC_PAGE_MAX / 2))) {
            break;
        }
    }
    return count;
}

/* Free pages when running low on memory */
void
lc_purgePages(struct gfs *gfs, bool force) {
    uint64_t count = 0;
    struct fs *fs;
    int i;

    if (lc_checkMemoryAvailable()) {
        return;
    }
    if (force) {
        pthread_mutex_lock(&gfs->gfs_lock);

        /* Wait for page purging to complete and memory to become available */
        if (gfs->gfs_tpurging) {
            pthread_cond_wait(&gfs->gfs_mcond, &gfs->gfs_lock);
            pthread_mutex_unlock(&gfs->gfs_lock);
            return;
        }
    } else {

        /* Return if pages are being purged */
        if (gfs->gfs_tpurging) {
            return;
        }
        if (pthread_mutex_trylock(&gfs->gfs_lock)) {
            return;
        }
        if (gfs->gfs_tpurging) {
            pthread_mutex_unlock(&gfs->gfs_lock);
            return;
        }
    }
    if (gfs->gfs_pcount < (LC_PAGE_MAX / 2)) {
        pthread_mutex_unlock(&gfs->gfs_lock);
        return;
    }

    /* Let a single thread do the job to avoid contention on locks */
    gfs->gfs_tpurging = true;
    for (i = 0; i <= gfs->gfs_scount; i++) {

        /* Start from a file system after the one processed last time */
        if (gfs->gfs_tpIndex > gfs->gfs_scount) {
            gfs->gfs_tpIndex = 0;
        }
        fs = gfs->gfs_fs[gfs->gfs_tpIndex++];

        /* A file system being removed when shared lock fails on it, so skip
         * those.
         */
        if (fs && !lc_tryLock(fs, false)) {
            pthread_mutex_unlock(&gfs->gfs_lock);

            /* Flush dirty pages first */
            if (fs->fs_pcount) {
                lc_flushDirtyInodeList(fs, true);
            }

            /* Purge clean pages for the tree */
            if ((fs->fs_parent == NULL) && fs->fs_bcache->lb_pcount) {
                count += lc_purgeTreePages(gfs, fs, false);
            }
            lc_unlock(fs);

            /* If memory is available now, wakeup waiting threads */
            if (lc_checkMemoryAvailable()) {
                pthread_cond_broadcast(&gfs->gfs_mcond);
            }
            pthread_mutex_lock(&gfs->gfs_lock);
        }
        if (gfs->gfs_pcount < (LC_PAGE_MAX / 2)) {
            break;
        }
    }
    gfs->gfs_tpurging = false;

    /* Wakeup threads waiting for memory to become available */
    pthread_cond_broadcast(&gfs->gfs_mcond);
    if (count) {
        gfs->gfs_purged += count;
    }
    pthread_mutex_unlock(&gfs->gfs_lock);
    lc_checkMemoryAvailable();
    lc_printf("Purged %ld pages\n", count);
}

/* Purge pages of inactive layers */
void *
lc_flusher(void *data) {
    const struct timespec interval = {10, 0};
    struct gfs *gfs = getfs();
    struct timeval now;
    uint64_t i, count;
    struct fs *fs;
    time_t sec;

    while (true) {
        nanosleep(&interval, NULL);
        if ((gfs->gfs_scount <= 1) || (gfs->gfs_pcount == 0)) {
            continue;
        }
        gettimeofday(&now, NULL);
        sec = now.tv_sec - 60;
        count = 0;

        pthread_mutex_lock(&gfs->gfs_lock);
        for (i = 1; i <= gfs->gfs_scount; i++) {
            fs = gfs->gfs_fs[i];

            /* Process layers which are inactive for some time */
            if ((fs == NULL) || !fs->fs_readOnly || fs->fs_snap ||
                (fs->fs_bcache->lb_pcount == 0) || (fs->fs_atime >= sec)) {
                continue;
            }

            /* If shared lock is not available, the layer is being deleted */
            if (!lc_tryLock(fs, false)) {
                pthread_mutex_unlock(&gfs->gfs_lock);
                if ((fs->fs_snap == NULL) && (fs->fs_atime < sec)) {
                    count += lc_purgeTreePages(gfs, fs->fs_rfs, true);
                }
                lc_unlock(fs);
                pthread_mutex_lock(&gfs->gfs_lock);
            }
        }
        if (count) {
            gfs->gfs_purged += count;
            pthread_mutex_unlock(&gfs->gfs_lock);
            lc_printf("purged %ld pages from idle layers\n", count);
        } else {
            pthread_mutex_unlock(&gfs->gfs_lock);
        }
    }
    return NULL;
}
