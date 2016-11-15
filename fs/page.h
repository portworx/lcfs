#ifndef _PAGE_H_
#define _PAGE_H_

#include "includes.h"

#define DFS_PAGE_HOLE       ((uint64_t)-1)

/* Initial size of the page hash table */
/* XXX This needs to consider available memory */
#define DFS_PCACHE_SIZE (1024 * 1024)

/* Page cache header */
struct pcache {

    /* Lock protecting the hash chain */
    pthread_mutex_t pc_lock;

    /* Page hash chains */
    struct page *pc_head;

    /* Count of pages in use */
    uint64_t pc_pcount;
};


/* Page structure used for caching a file system block */
struct page {

    /* Data associated with page of the file */
    char *p_data;

    /* Block mapping to */
    uint64_t p_block;

    /* Page cache the page belongs to */
    struct pcache *p_pcache;

    /* Reference count on this page */
    uint64_t p_refCount;

    /* Next page in block hash table */
    struct page *p_cnext;

    /* Next page in file system dirty list */
    struct page *p_dnext;

    /* Next page in free list */
    struct page *p_fnext;

    /* Previous page in free list */
    struct page *p_fprev;

    /* Lock protecting operations on the page */
    pthread_mutex_t p_lock;

    /* Lock protecting data read */
    pthread_mutex_t p_dlock;

    /* Set if data is valid */
    uint8_t p_dvalid;

} __attribute__((packed));

#endif
