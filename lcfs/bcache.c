#include "includes.h"

/* Return the hash number for the block number provided */
static inline int
lc_pageBlockHash(struct fs *fs, uint64_t block) {
    assert(block);
    assert(block != LC_INVALID_BLOCK);
    return block % fs->fs_pcacheSize;
}

/* Allocate a new page */
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
    pthread_mutex_init(&page->p_dlock, NULL);
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
        lc_free(fs->fs_rfs, page->p_data, LC_BLOCK_SIZE, LC_MEMTYPE_DATA);
    }
    pthread_mutex_destroy(&page->p_dlock);
    lc_free(fs->fs_rfs, page, sizeof(struct page), LC_MEMTYPE_PAGE);
    __sync_sub_and_fetch(&gfs->gfs_pcount, 1);
}

/* Allocate and initialize page block hash table */
void
lc_pcache_init(struct fs *fs, uint32_t count, uint32_t lcount) {
    struct pcache *pcache;
    int i;

    pcache = lc_malloc(fs, sizeof(struct pcache) * count, LC_MEMTYPE_PCACHE);
    memset(pcache, 0, sizeof(struct pcache) * count);
    fs->fs_pcacheLocks = lc_malloc(fs, sizeof(pthread_mutex_t) * lcount,
                               LC_MEMTYPE_PCLOCK);
    for (i = 0; i < lcount; i++) {
        pthread_mutex_init(&fs->fs_pcacheLocks[i], NULL);
    }
    fs->fs_pcacheLockCount = lcount;
    fs->fs_pcache = pcache;
    fs->fs_pcacheSize = count;
}

/* Find the lock hash for the list */
static inline uint32_t
lc_lockHash(struct fs *fs, uint64_t hash) {
    return hash % fs->fs_pcacheLockCount;
}

/* Lock a hash list */
static void
lc_pcLockHash(struct fs *fs, uint64_t hash) {
    uint32_t lock = lc_lockHash(fs, hash);

    pthread_mutex_lock(&fs->fs_pcacheLocks[lock]);
}

/* Lock a hash list */
static void
lc_pcUnLockHash(struct fs *fs, uint64_t hash) {
    uint32_t lock = lc_lockHash(fs, hash);

    pthread_mutex_unlock(&fs->fs_pcacheLocks[lock]);
}

/* Remove pages from page cache and free the hash table */
void
lc_destroy_pages(struct gfs *gfs, struct fs *fs, struct pcache *pcache,
                 bool remove) {
    uint64_t i, count = 0, pcount;
    struct page *page;

    for (i = 0; i < fs->fs_pcacheSize; i++) {
        pcount = 0;
        lc_pcLockHash(fs, i);
        while ((page = pcache[i].pc_head)) {
            pcache[i].pc_head = page->p_cnext;
            page->p_block = LC_INVALID_BLOCK;
            page->p_cnext = NULL;
            page->p_dvalid = 0;
            lc_freePage(gfs, fs, page);
            pcount++;
        }
        assert(pcount == pcache[i].pc_pcount);
        assert(pcache[i].pc_head == NULL);
        lc_pcUnLockHash(fs, i);
        count += pcount;
    }
    lc_free(fs, pcache, sizeof(struct pcache) * fs->fs_pcacheSize,
            LC_MEMTYPE_PCACHE);
    for (i = 0; i < fs->fs_pcacheLockCount; i++) {
        pthread_mutex_destroy(&fs->fs_pcacheLocks[i]);
    }
    lc_free(fs, fs->fs_pcacheLocks,
            sizeof(pthread_mutex_t) * fs->fs_pcacheLockCount,
            LC_MEMTYPE_PCLOCK);
    if (count && remove) {
        __sync_add_and_fetch(&gfs->gfs_preused, count);
    }
}

/* Release a page */
void
lc_releasePage(struct gfs *gfs, struct fs *fs, struct page *page, bool read) {
    struct page *cpage, *prev = NULL, *fpage = NULL, *fprev = NULL;
    struct pcache *pcache = fs->fs_pcache;
    uint64_t hash, hit, count = 0;

    hash = lc_pageBlockHash(fs, page->p_block);
    lc_pcLockHash(fs, hash);
    assert(page->p_refCount > 0);
    page->p_refCount--;

    /* If page was read, increment hit count */
    if (page->p_nocache && (page->p_refCount == 0)) {
        if (pcache[hash].pc_head != page) {
            cpage = pcache[hash].pc_head;
            while (cpage) {
                if (cpage->p_cnext == page) {
                    fprev = cpage;
                    break;
                }
                cpage = cpage->p_cnext;
            }
            assert(fprev);
        }
        fpage = page;
    } else if (read) {
        page->p_hitCount++;
    }

    /* Free a page if page cache is above limit */
    if ((fpage == NULL) && (lc_lowMemory() ||
                            (pcache[hash].pc_pcount >
                             (LC_PAGE_MAX / fs->fs_pcacheSize)))) {
        cpage = pcache[hash].pc_head;
        hit = page->p_hitCount;

        /* Look for a page with lowest hit count */
        while (cpage) {
            if ((cpage->p_refCount == 0) && (cpage->p_hitCount <= hit)) {
                fprev = prev;
                fpage = cpage;
                hit = cpage->p_hitCount;
            }
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
        if (fprev == NULL) {
            pcache[hash].pc_head = fpage->p_cnext;
        } else {
            fprev->p_cnext = fpage->p_cnext;
        }
        fpage->p_block = LC_INVALID_BLOCK;
        fpage->p_cnext = NULL;
        assert(pcache[hash].pc_pcount > 0);
        pcache[hash].pc_pcount--;
    }
    lc_pcUnLockHash(fs, hash);
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
    lc_waitMemory(false);
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

/* Add a page to page block hash list */
void
lc_addPageBlockHash(struct gfs *gfs, struct fs *fs,
                    struct page *page, uint64_t block) {
    struct pcache *pcache = fs->fs_pcache;
    int hash = lc_pageBlockHash(fs, block);
    struct page *cpage;

    assert(page->p_block == LC_INVALID_BLOCK);
    page->p_block = block;
    lc_pcLockHash(fs, hash);
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
    lc_pcUnLockHash(fs, hash);
}

/* Lookup a page in the block hash */
struct page *
lc_getPage(struct fs *fs, uint64_t block, bool read) {
    int hash = lc_pageBlockHash(fs, block);
    struct pcache *pcache = fs->fs_pcache;
    struct page *page, *new = NULL;
    struct gfs *gfs = fs->fs_gfs;
    bool hit = false;

    assert(block);
    assert(block != LC_PAGE_HOLE);

retry:
    lc_pcLockHash(fs, hash);
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
    lc_pcUnLockHash(fs, hash);

    /* If no page is found, allocate one and retry */
    if (page == NULL) {
        new = lc_newPage(gfs, fs);
        assert(!new->p_dvalid);
        goto retry;
    }

    /* If raced with another thread, free the unused page */
    if (new) {
        new->p_refCount = 0;
        lc_freePage(gfs, fs, new);
    }

    /* If page is missing data, read from disk */
    if (read && !page->p_dvalid) {

        /* XXX Use a shared lock instead of a per page one */
        pthread_mutex_lock(&page->p_dlock);
        if (!page->p_dvalid) {
            lc_mallocBlockAligned(fs->fs_rfs, (void **)&page->p_data,
                                  LC_MEMTYPE_DATA);
            lc_readBlock(gfs, fs, block, page->p_data);
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
struct page *
lc_getPageNew(struct gfs *gfs, struct fs *fs, uint64_t block, char *data) {
    struct page *page = lc_getPage(fs, block, false);

    assert(page->p_refCount == 1);
    if (page->p_data) {
        lc_free(fs->fs_rfs, page->p_data, LC_BLOCK_SIZE, LC_MEMTYPE_DATA);
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
        if ((fs->fs_wpcount == 0) && (fs->fs_dpcount == 0) &&
            fs->fs_fdextents) {
            pthread_mutex_lock(&fs->fs_alock);
            extents = fs->fs_fdextents;
            fs->fs_fdextents = NULL;
            pthread_mutex_unlock(&fs->fs_alock);
        }
        pthread_mutex_unlock(&fs->fs_plock);
        if (extents) {
            lc_blockFreeExtents(fs, extents, !fs->fs_removed, false, true);
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
lc_purgeTreePages(struct gfs *gfs, struct fs *fs) {
    struct pcache *pcache = fs->fs_pcache;
    struct page *page, *prev, *next;;
    uint64_t i, j, count = 0;

    for (j = 0; j < fs->fs_pcacheSize; j++) {
        i = fs->fs_purgeIndex++;
        if (fs->fs_purgeIndex >= fs->fs_pcacheSize) {
             fs->fs_purgeIndex = 0;
        }
        assert(i < fs->fs_pcacheSize);
        if (pcache[i].pc_pcount == 0) {
            continue;
        }
        lc_pcLockHash(fs, i);
        page = pcache[i].pc_head;
        prev = NULL;
        while (page) {
            if (page->p_refCount == 0) {
                if (prev == NULL) {
                    pcache[i].pc_head = page->p_cnext;
                } else {
                    prev->p_cnext = page->p_cnext;
                }
                next = page->p_cnext;
                //lc_printf("fs %p Freeing page %p, block %ld\n", fs, page, page->p_block);
                page->p_cnext = NULL;
                page->p_block = LC_INVALID_BLOCK;
                page->p_dvalid = 0;
                lc_freePage(gfs, fs, page);
                assert(pcache[i].pc_pcount > 0);
                pcache[i].pc_pcount--;
                count++;
                page = next;
                continue;
            }
            prev = page;
            page = page->p_cnext;
        }
        lc_pcUnLockHash(fs, i);
        if ((gfs->gfs_pcount < LC_PAGE_MAX) && !lc_lowMemory()) {
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

    if (force) {
        pthread_mutex_lock(&gfs->gfs_lock);
        if (gfs->gfs_tpurging) {
            pthread_cond_wait(&gfs->gfs_mcond, &gfs->gfs_lock);
            pthread_mutex_unlock(&gfs->gfs_lock);
            return;
        }
    } else {
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
    gfs->gfs_tpurging = true;
    for (i = 0; i <= gfs->gfs_scount; i++) {
        if (gfs->gfs_tpIndex > gfs->gfs_scount) {
            gfs->gfs_tpIndex = 0;
        }
        fs = gfs->gfs_fs[gfs->gfs_tpIndex++];
        if (fs && !lc_tryLock(fs, false)) {
            pthread_mutex_unlock(&gfs->gfs_lock);
            if (fs->fs_pcount) {
                lc_flushDirtyInodeList(fs, true);
            }
            if (fs->fs_parent == NULL) {
                count += lc_purgeTreePages(gfs, fs);
            }
            lc_unlock(fs);
            lc_checkMemoryAvailable(true);
            if (!lc_lowMemory()) {
                pthread_cond_broadcast(&gfs->gfs_mcond);
            }
            pthread_mutex_lock(&gfs->gfs_lock);
        }
        if ((gfs->gfs_pcount < LC_PAGE_MAX) && !lc_lowMemory()) {
            break;
        }
    }
    gfs->gfs_tpurging = false;
    pthread_cond_broadcast(&gfs->gfs_mcond);
    if (count) {
        gfs->gfs_purged += count;
    }
    pthread_mutex_unlock(&gfs->gfs_lock);
    lc_checkMemoryAvailable(true);
}
