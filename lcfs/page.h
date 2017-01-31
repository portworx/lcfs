#ifndef _PAGE_H_
#define _PAGE_H_

#include "includes.h"

/* HOLE representation for a page of an inode */
#define LC_PAGE_HOLE       ((uint64_t)-1)

/* Initial size of the page hash table */
/* XXX This needs to consider available memory */
#define LC_PCACHE_SIZE_MIN  1024
#define LC_PCACHE_SIZE      (128 * 1024)

/* Number of locks for the block cache hash lists */
#define LC_PCLOCK_COUNT     1024

/* Number of hash lists for the dirty pages */
/* XXX Adjust this with size of the file */
#define LC_PAGECACHE_SIZE  32

/* Maximum number of blocks grouped in a single read request */
#define LC_READ_CLUSTER_SIZE    32

/* Maximum number of blocks grouped in a single write request */
#define LC_WRITE_CLUSTER_SIZE   256

/* Maximum memory in bytes allowed for data pages */
#define LC_PCACHE_MEMORY        (512ull * 1024ull * 1024ull)

/* Percentage of memory allowed above LC_PCACHE_MEMORY before threads are
 * blocked.
 */
#define LC_PURGE_TARGET         20

/* Minimum amount of total system memory which can be used for data pages if
 * system does not have LC_PCACHE_MEMORY bytes of memory.
 */
#define LC_PCACHE_MEMORY_MIN    25

/* Maximum number of dirty pages a file could have before flushing triggered */
#define LC_MAX_FILE_DIRTYPAGES  131072

/* Maximum number of dirty pages a layer could have before flushing triggered
 */
#define LC_MAX_LAYER_DIRTYPAGES 524288

/* Number of minimum blocks a file need to grow before it is converted to use a
 * hash scheme for dirty pages.
 */
#define LC_DHASH_MIN            1024

/* Time in seconds background flusher is woken up */
#define LC_FLUSH_INTERVAL       120

/* Time in seconds background cleaner is woken up */
#define LC_CLEAN_INTERVAL       60

/* Time in seconds before flusher kicks in on a newly created layer */
#define LC_FLUSH_TIME           120

/* Time in seconds before cleaner kicks in on a newly created layer */
#define LC_PURGE_TIME           30

/* Page cache header */
struct pcache {
    /* Page hash chains */
    struct page *pc_head;

    /* Count of pages in use */
    uint32_t pc_pcount;
} __attribute__((packed));


/* Block cache for a layer tree */
struct lbcache {

    /* Block cache hash headers */
    struct pcache *lb_pcache;

    /* Locks for the page cache lists */
    pthread_mutex_t *lb_pcacheLocks;

    /* Locks for serializing I/Os */
    pthread_mutex_t *lb_pioLocks;

    /* Number of hash lists in pcache */
    uint32_t lb_pcacheSize;

    /* Number of page cache locks */
    uint32_t lb_pcacheLockCount;

    /* Count of clean pages */
    uint64_t lb_pcount;
} __attribute__((packed));

/* Page structure used for caching a file system block */
struct page {

    /* Data associated with page of the file */
    char *p_data;

    /* Block mapping to */
    uint64_t p_block:48;

    /* Layer index allocated this block */
    uint64_t p_lindex:16;

    /* Reference count on this page */
    uint32_t p_refCount;

    /* Page cache hitcount */
    uint32_t p_hitCount:30;

    /* Set if data is valid */
    uint32_t p_nocache:1;

    /* Set if data is valid */
    uint32_t p_dvalid:1;

    /* Next page in block hash table */
    struct page *p_cnext;

    /* Next page in file system dirty list */
    struct page *p_dnext;
};

/* Page structure used for caching dirty pages of an inode
 * when the inode is using an array indexed by page number.
 */
struct dpage {

    /* Data associated with page of the file */
    char *dp_data;

    /* Offset at which valid data starts */
    uint16_t dp_poffset;

    /* Size of valid data starting from dp_poffset */
    uint16_t dp_psize:15;

    /* Read while dirty */
    uint16_t dp_pread:1;
} __attribute__((packed));

/* Page structure used for caching dirty pages of an inode
 * when the inode is using a hash table indexed by page number.
 */
struct dhpage {
    /* Page number */
    uint64_t dh_pg;

    /* Next in the hash chain */
    struct dhpage *dh_next;

    /* Details on data */
    struct dpage dh_page;
} __attribute__((packed));

#endif
