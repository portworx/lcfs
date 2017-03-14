#include "includes.h"

/* Set for tracking memory allocation and free operations */
#ifdef LC_MEMSTATS_ENABLE
static bool memStatsEnabled = true;
#else
static bool memStatsEnabled = false;
#endif

static struct lc_memory {

    /* Total memory currently used for data pages */
    uint64_t m_totalMemory;

    /* Maximum memory that can be used for data pages */
    uint64_t m_dataMemory;

    /* Amount of memory for data pages targetted by cleaner */
    uint64_t m_purgeMemory;

    /* Memory allocated globally */
    uint64_t m_globalMemory;

    /* Count of global mallocs */
    uint64_t m_globalMalloc;

    /* Count of global free */
    uint64_t m_globalFree;
} lc_mem;

/* Type of malloc requests */
static const char *mrequests[] = {
    "GFS",
    "DIRENT",
    "DCACHE",
    "ICACHE",
    "INODE",
    "LBCACHE",
    "PCACHE",
    "PCLOCK",
    "EXTENT",
    "BLOCK",
    "PAGE",
    "DATA",
    "DPAGEHASH",
    "HPAGE",
    "XATTR",
    "XATTRNAME",
    "XATTRVALUE",
    "XATTRBUF",
    "XATTRINODE",
    "CFILE",
    "CDIR",
    "PATH",
    "HLINKS",
    "SYMLINK",
    "RWLOCK",
    "STATS",
};

/* Initialize limit based on available memory */
void
lc_memoryInit(void) {
    uint64_t totalram = lc_getTotalMemory();

    lc_mem.m_purgeMemory = LC_PCACHE_MEMORY;
    if (totalram < lc_mem.m_purgeMemory) {
        lc_mem.m_purgeMemory = (totalram * LC_PCACHE_MEMORY_MIN) / 100;
    }
    lc_mem.m_dataMemory = (lc_mem.m_purgeMemory * (100 + LC_PURGE_TARGET))
                          / 100;
    lc_printf("Maximum memory allowed for data pages %ld MB\n",
              lc_mem.m_purgeMemory / (1024 * 1024));
}

/* Check memory usage for data pages is under limit or not */
bool
lc_checkMemoryAvailable(bool flush) {
    return lc_mem.m_totalMemory < (flush ?
                                   lc_mem.m_purgeMemory : lc_mem.m_dataMemory);
}

/* Flush dirty pages and purge cache entries when running low on memory */
void
lc_waitMemory(bool wait) {
    struct gfs *gfs = getfs();

    /* If memory used for data usage is above limit, Flush dirty pages and
     * purge cache entries.
     */
    if (!lc_checkMemoryAvailable(false)) {
        lc_wakeupCleaner(gfs, wait);
    }
}

/* Update memory stats */
static inline void
lc_memStatsUpdate(struct fs *fs, size_t size, bool alloc,
                  enum lc_memTypes type) {
    uint64_t freed;

    /* Update memory usage for data pages */
    if ((type == LC_MEMTYPE_PAGE) || (type == LC_MEMTYPE_DATA) ||
        (type == LC_MEMTYPE_BLOCK)) {
        if (alloc) {
            __sync_add_and_fetch(&lc_mem.m_totalMemory, size);
        } else {
            freed = __sync_fetch_and_sub(&lc_mem.m_totalMemory, size);
            assert(freed >= size);
        }
    }

    /* Skip memory tracking if not enabled */
    if (!memStatsEnabled) {
        return;
    }

    //lc_printf("lc_memStatsUpdate: size %ld for %s %s\n", size, mrequests[type], alloc ? "allocated" : "freed");
    if (fs) {

        /* Per layer stats */
        if (alloc) {
            __sync_add_and_fetch(&fs->fs_memory, size);
            __sync_add_and_fetch(&fs->fs_malloc[type], 1);
        } else {
            freed = __sync_fetch_and_sub(&fs->fs_memory, size);
            assert(freed >= size);
            __sync_add_and_fetch(&fs->fs_free[type], 1);
        }
    } else {

        /* Global stats */
        assert(type == LC_MEMTYPE_GFS);
        if (alloc) {
            __sync_add_and_fetch(&lc_mem.m_globalMemory, size);
            __sync_add_and_fetch(&lc_mem.m_globalMalloc, 1);
        } else {
            freed = __sync_fetch_and_sub(&lc_mem.m_globalMemory, size);
            assert(freed >= size);
            __sync_add_and_fetch(&lc_mem.m_globalFree, 1);
        }
    }
}

/* Substract total memory usage */
void
lc_memUpdateTotal(struct fs *fs, size_t size) {
    if (!memStatsEnabled) {
        return;
    }
    __sync_fetch_and_sub(&fs->fs_memory, size);
}

/* Transfer some memory from a layer to another layer */
void
lc_memTransferCount(struct fs *fs, struct fs *rfs, uint64_t count,
                    enum lc_memTypes type) {
    uint64_t size, freed;

    /* This is done when a dirty page is moved to shared block page cache or
     * when some extents are moved from one layer to another.
     */
    if (memStatsEnabled && (fs != rfs)) {
        assert((type == LC_MEMTYPE_DATA) || (type == LC_MEMTYPE_EXTENT));
        size = count * ((type == LC_MEMTYPE_DATA) ?
                        LC_BLOCK_SIZE : sizeof(struct extent));
        __sync_add_and_fetch(&rfs->fs_memory, size);
        freed = __sync_fetch_and_sub(&fs->fs_memory, size);
        assert(freed >= size);
        __sync_add_and_fetch(&fs->fs_free[type], count);
        __sync_add_and_fetch(&rfs->fs_malloc[type], count);
    }
}

/* Swap memory allocated for extents */
void
lc_memTransferExtents(struct gfs *gfs, struct fs *fs, struct fs *cfs) {
    uint64_t i, j;

    if (!memStatsEnabled) {
        return;
    }
    i = lc_countExtents(gfs, fs->fs_aextents, NULL);
    i += lc_countExtents(gfs, fs->fs_fextents, NULL);
    i += lc_countExtents(gfs, fs->fs_mextents, NULL);
    i += lc_countExtents(gfs, fs->fs_rextents, NULL);

    j = lc_countExtents(gfs, cfs->fs_aextents, NULL);
    j += lc_countExtents(gfs, cfs->fs_fextents, NULL);
    j += lc_countExtents(gfs, cfs->fs_mextents, NULL);
    j += lc_countExtents(gfs, cfs->fs_rextents, NULL);
    if (i == j) {
        return;
    }
    if (i > j) {
        lc_memTransferCount(fs, cfs, i - j, LC_MEMTYPE_EXTENT);
    } else {
        lc_memTransferCount(cfs, fs, j - i, LC_MEMTYPE_EXTENT);
    }
}

/* Allocart requested amount of memory for the specified purpose */
void *
lc_malloc(struct fs *fs, size_t size, enum lc_memTypes type) {
    lc_memStatsUpdate(fs, size, true, type);
    return malloc(size);
}

/* Allocate block aligned memory, needed for direct I/O */
void
lc_mallocBlockAligned(struct fs *fs, void **memptr, enum lc_memTypes type) {
    int err = posix_memalign(memptr, LC_BLOCK_SIZE, LC_BLOCK_SIZE);

    assert(err == 0);
    lc_memStatsUpdate(fs, LC_BLOCK_SIZE, true, type);
}

/* Release previously allocated memory */
void
lc_free(struct fs *fs, void *ptr, size_t size, enum lc_memTypes type) {
    assert(size || (type == LC_MEMTYPE_GFS));
    free(ptr);
    lc_memStatsUpdate(fs, size, false, type);
}

/* Move previously allocated memory from one layer to another */
void
lc_memMove(struct fs *from, struct fs *to, size_t size,
           enum lc_memTypes type) {
    if (memStatsEnabled) {
        lc_memStatsUpdate(from, size, false, type);
        lc_memStatsUpdate(to, size, true, type);
    }
}

/* Check for memory leak */
void
lc_checkMemStats(struct fs *fs, bool unmount) {
    enum lc_memTypes i;

    assert(!unmount || (lc_mem.m_totalMemory == 0));
    if (!memStatsEnabled) {
        return;
    }
    for (i = LC_MEMTYPE_GFS + 1; i < LC_MEMTYPE_MAX; i++) {
        assert(fs->fs_malloc[i] == fs->fs_free[i]);
    }
    assert(fs->fs_memory == 0);
}

/* Display global memory stats */
void
lc_displayGlobalMemStats() {
    if (lc_mem.m_globalMemory) {
        printf("\tGlobal Allocated %ld Freed %ld Total in use %ld bytes\n",
               lc_mem.m_globalMalloc, lc_mem.m_globalFree,
               lc_mem.m_globalMemory);
    }
    if (lc_mem.m_totalMemory) {
        printf("Total memory used for pages %ld\n", lc_mem.m_totalMemory);
    }
}

/* Display memory stats */
void
lc_displayMemStats(struct fs *fs) {
    struct timeval now;
    enum lc_memTypes i;

    if (!memStatsEnabled) {
        return;
    }
    if (fs->fs_memory == 0) {
        return;
    }
    gettimeofday(&now, NULL);
    printf("\n\nMemory Stats for file system %p with root %ld index %d at "
           "%s\n", fs, fs->fs_root, fs->fs_gindex, ctime(&now.tv_sec));
    for (i = LC_MEMTYPE_GFS + 1; i < LC_MEMTYPE_MAX; i++) {
        if (fs->fs_malloc[i]) {
            printf("\t%s Allocated %ld Freed %ld in use %ld\n",
                   mrequests[i], fs->fs_malloc[i], fs->fs_free[i],
                   fs->fs_malloc[i] - fs->fs_free[i]);
        }
    }
    printf("\n\tTotal memory in use %ld bytes\n\n", fs->fs_memory);
}
