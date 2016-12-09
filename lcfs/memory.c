#include "includes.h"

static bool memStatsEnabled = true;
static uint64_t global_memory = 0, global_malloc = 0, global_free = 0;

/* Type of malloc requests */
static const char *mrequests[] = {
    "GFS",
    "DIRENT",
    "ICACHE",
    "INODE",
    "PCACHE",
    "ILOCK",
    "EXTENT",
    "BLOCK",
    "PAGE",
    "DATA",
    "DPAGEHASH",
    "XATTR",
    "XATTRNAME",
    "XATTRVALUE",
    "XATTRBUF",
    "XATTRINODE",
    "STATS",
};

/* Update memory stats */
static inline void
lc_memStatsUpdate(struct fs *fs, size_t size, bool alloc,
                  enum lc_memTypes type) {
    uint64_t freed;

    if (!memStatsEnabled) {
        return;
    }

    //lc_printf("lc_memStatsUpdate: size %ld for %s %s\n", size, mrequests[type], alloc ? "allocated" : "freed");
    if (fs) {
        if (alloc) {
            __sync_add_and_fetch(&fs->fs_memory, size);
            __sync_add_and_fetch(&fs->fs_malloc[type], 1);
        } else {
            freed = __sync_fetch_and_sub(&fs->fs_memory, size);
            assert(freed >= size);
            __sync_add_and_fetch(&fs->fs_free[type], 1);
        }
    } else {
        assert(type == LC_MEMTYPE_GFS);
        if (alloc) {
            __sync_add_and_fetch(&global_memory, size);
            __sync_add_and_fetch(&global_malloc, 1);
        } else {
            freed = __sync_fetch_and_sub(&global_memory, size);
            assert(freed >= size);
            __sync_add_and_fetch(&global_free, 1);
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

    if (memStatsEnabled && (fs != rfs)) {
        size = count * LC_BLOCK_SIZE;
        __sync_add_and_fetch(&rfs->fs_memory, size);
        freed = __sync_fetch_and_sub(&fs->fs_memory, size);
        assert(freed >= size);
        __sync_add_and_fetch(&fs->fs_free[LC_MEMTYPE_DATA], count);
        __sync_add_and_fetch(&rfs->fs_malloc[LC_MEMTYPE_DATA], count);
    }
}

/* Allocarte requested amount of memory for the specified purpose */
void *
lc_malloc(struct fs *fs, size_t size, enum lc_memTypes type) {
    lc_memStatsUpdate(fs, size, true, type);
    return malloc(size);
}

/* Allocate block aligned memory */
void
lc_mallocBlockAligned(struct fs *fs, void **memptr, enum lc_memTypes type) {
    int err = posix_memalign(memptr, LC_BLOCK_SIZE, LC_BLOCK_SIZE);

    assert(err == 0);
    lc_memStatsUpdate(fs, LC_BLOCK_SIZE, true, type);
}

/* Release previously allocated memory */
void
lc_free(struct fs *fs, void *ptr, size_t size, enum lc_memTypes type) {
    free(ptr);
    lc_memStatsUpdate(fs, size, false, type);
}

/* Check for memory leak */
void
lc_checkMemStats(struct fs *fs) {
    enum lc_memTypes i;

    if (!memStatsEnabled) {
        return;
    }
    if (fs->fs_memory == 0) {
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
    if (global_memory) {
        printf("\tGlobal Allocated %ld Freed %ld Total in use %ld bytes\n",
               global_malloc, global_free, global_memory);
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
    printf("\n\nMemory Stats for file system %p with root %ld index %d at %s\n",
           fs, fs->fs_root, fs->fs_gindex, ctime(&now.tv_sec));
    for (i = LC_MEMTYPE_GFS + 1; i < LC_MEMTYPE_MAX; i++) {
        if (fs->fs_malloc[i]) {
            printf("\t%s Allocated %ld Freed %ld\n",
                   mrequests[i], fs->fs_malloc[i], fs->fs_free[i]);
        }
    }
    printf("\n\tTotal memory in use %ld bytes\n\n", fs->fs_memory);
}
