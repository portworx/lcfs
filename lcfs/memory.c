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
    "STATS",
};

/* Initialize limit based on available memory */
void
lc_memoryInit(void) {
    struct sysinfo info;

    lc_mem.m_dataMemory = LC_PCACHE_MEMORY;
    sysinfo(&info);
    if (info.totalram < lc_mem.m_dataMemory) {
        lc_mem.m_dataMemory = (info.totalram * LC_PCACHE_MEMORY_MIN) / 100;
    }
    lc_printf("Maximum memory allowed for data pages %ld MB\n",
              lc_mem.m_dataMemory / (1024 * 1024));
}

/* Check memory usage for data pages is under limit or not */
bool
lc_checkMemoryAvailable() {
    return lc_mem.m_totalMemory < lc_mem.m_dataMemory;
}

/* Flush dirty pages and purge cache entries when running low on memory */
void
lc_waitMemory(void) {
    struct gfs *gfs = getfs();

    /* If memory used for data usage is above limit, Flush dirty pages and
     * purge cache entries.
     */
    if (!lc_checkMemoryAvailable()) {
        lc_purgePages(gfs, true);
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

/* Transfer some memory from a layer it its base layer */
void
lc_memTransferCount(struct fs *fs, uint64_t count) {
    struct fs *rfs = fs->fs_rfs;
    uint64_t size, freed;

    /* This is done when a dirty page is moved to shared block page cache */
    if (memStatsEnabled && (fs != rfs)) {
        size = count * LC_BLOCK_SIZE;
        __sync_add_and_fetch(&rfs->fs_memory, size);
        freed = __sync_fetch_and_sub(&fs->fs_memory, size);
        assert(freed >= size);
        __sync_add_and_fetch(&fs->fs_free[LC_MEMTYPE_DATA], count);
        __sync_add_and_fetch(&rfs->fs_malloc[LC_MEMTYPE_DATA], count);
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
