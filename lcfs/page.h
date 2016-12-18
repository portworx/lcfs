#ifndef _PAGE_H_
#define _PAGE_H_

#include "includes.h"

#define LC_PAGE_HOLE       ((uint64_t)-1)

/* Initial size of the page hash table */
/* XXX This needs to consider available memory */
#define LC_PCACHE_SIZE_MIN  1024
#define LC_PCACHE_SIZE      (1024 * 1024)
#define LC_PAGE_MAX         1200000
static_assert(LC_PAGE_MAX >= LC_PCACHE_SIZE, "LC_PAGE_MAX <= LC_PCACHE_SIZE");

#define LC_PAGECACHE_SIZE  32
#define LC_CLUSTER_SIZE    256

#define LC_PCACHE_MEMORY        (512 * 1024 * 1024)
#define LC_MAX_FILE_DIRTYPAGES  131072
#define LC_MAX_LAYER_DIRTYPAGES 524288
#define LC_FLUSHER_MAX          10

#define LC_CACHE_PURGE_CHECK_MAX 10

#define LC_DHASH_MIN            1024

/* Page cache header */
struct pcache {

    /* Lock protecting the hash chain */
    pthread_mutex_t pc_lock;

    /* Page hash chains */
    struct page *pc_head;

    /* Count of pages in use */
    uint32_t pc_pcount;
} __attribute__((packed));


/* Page structure used for caching a file system block */
struct page {

    /* Data associated with page of the file */
    char *p_data;

    /* Block mapping to */
    uint64_t p_block;

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

    /* Lock protecting data read */
    pthread_mutex_t p_dlock;

} __attribute__((packed));

/* Page structure used for caching dirty pages of an inode
 * when the inode is using an array indexed by page number.
 */
struct dpage {

    /* Data associated with page of the file */
    char *dp_data;

    /* Offset at which valid data starts */
    uint16_t dp_poffset;

    /* Size of valid data starting from dp_poffset */
    uint16_t dp_psize;
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
