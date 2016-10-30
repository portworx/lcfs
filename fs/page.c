#include "includes.h"

static char dfs_zPage[DFS_BLOCK_SIZE];
#define DFS_PAGECACHE_SIZE 128

/* Page structure used for caching a file system block */
struct page {

    /* Data associated with page of the file */
    char *p_data;

    /* Page is shared with other inodes in the snapshot chain */
    bool p_shared;
} __attribute__((packed));

/* Return the requested page if allocated already */
static struct page *
dfs_findPage(struct inode *inode, uint64_t pg) {
    return (pg < inode->i_pcount) ? &inode->i_page[pg] : NULL;
}

/* Create a new page chain for the inode, copying existing page structures */
static void
dfs_copyPages(struct inode *inode) {
    struct page *page, *opage;
    uint64_t i, count = 0;

    opage = inode->i_page;
    inode->i_page = malloc(inode->i_pcount * sizeof(struct page));
    for (i = 0; i < inode->i_pcount; i++) {
        page = &inode->i_page[i];
        page->p_data = opage[i].p_data;
        if (page->p_data) {
            page->p_shared = true;
            count++;
        } else {
            page->p_shared = false;
        }
    }
    assert(inode->i_stat.st_blocks == count);
    inode->i_shared = false;
}

/* Add the page to bufvec */
static inline void
dfs_updateVec(struct page *page, struct fuse_bufvec *bufv,
               off_t poffset, size_t psize) {
    bufv->buf[bufv->count].mem = &page->p_data[poffset];
    bufv->buf[bufv->count].size = psize;
    bufv->count++;
}

/* Allocate/extend inode page table */
void
dfs_inodeAllocPages(struct inode *inode) {
    uint64_t lpage, count, size, tsize;
    struct page *page;

    assert(!inode->i_shared);
    lpage = (inode->i_stat.st_size + DFS_BLOCK_SIZE - 1) / DFS_BLOCK_SIZE;
    if (inode->i_pcount < lpage) {
        count = inode->i_pcount ?  (inode->i_pcount * 2) : DFS_PAGECACHE_SIZE;
        while (count < lpage) {
            count *= 2;
        }
        tsize = count * sizeof(struct page);
        page = malloc(tsize);
        if (inode->i_pcount) {
            size = inode->i_pcount * sizeof(struct page);
            memcpy(page, inode->i_page, size);
            memset(&page[inode->i_pcount], 0, tsize - size);
            free(inode->i_page);
        } else {
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
            struct fuse_bufvec *bufv) {
    struct page *page;
    bool newblock;
    char *data;

    assert(pg < inode->i_pcount);
    page = dfs_findPage(inode, pg);
    if (page->p_data) {

        /* Allocate a new page if current page is shared */
        newblock = page->p_shared;
        if (page->p_shared) {
            data = page->p_data;
            page->p_data = malloc(DFS_BLOCK_SIZE);
            if (poffset != 0) {
                memcpy(page->p_data, data, poffset);
            }
            if ((poffset + psize) != DFS_BLOCK_SIZE) {
                memcpy(&page->p_data[(poffset + psize)], data,
                       DFS_BLOCK_SIZE - (poffset + psize));
            }
            page->p_shared = false;
        }
        assert(!page->p_shared);
        dfs_updateVec(page, bufv, poffset, psize);
        return newblock ? 1 : 0;
    }

    /* Allocate a new page */
    page->p_data = malloc(DFS_BLOCK_SIZE);
    if (poffset != 0) {
        memset(page->p_data, 0, poffset);
    }
    if ((poffset + psize) != DFS_BLOCK_SIZE) {
        memset(&page->p_data[(poffset + psize)], 0,
               DFS_BLOCK_SIZE - (poffset + psize));
    }
    dfs_updateVec(page, bufv, poffset, psize);
    inode->i_stat.st_blocks++;
    return 1;
}

/* Update pages of a file with provided data */
int
dfs_addPages(struct inode *inode, off_t off, size_t size,
             struct fuse_bufvec *bufv, struct fuse_bufvec *dst) {
    size_t wsize = size, psize;
    uint64_t page, spage;
    off_t poffset;
    int count = 0;

    assert(S_ISREG(inode->i_stat.st_mode));

    /* Copy page headers if page chain is shared */
    if (inode->i_shared) {
        dfs_copyPages(inode);
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
        count += dfs_addPage(inode, page, poffset, psize, dst);
        page++;
        wsize -= psize;
    }
    wsize = fuse_buf_copy(dst, bufv, FUSE_BUF_SPLICE_NONBLOCK);
    assert(wsize == size);
    return count;
}

/* Read specified pages of a file */
void
dfs_readPages(struct inode *inode, off_t soffset, off_t endoffset,
              struct fuse_bufvec *bufv) {
    uint64_t pg = soffset / DFS_BLOCK_SIZE;
    off_t poffset, off = soffset, roff = 0;
    size_t psize, rsize = endoffset - off;
    struct page *page = NULL;
    int i = 0;

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

        page = dfs_findPage(inode, pg);
        if (page->p_data) {
            bufv->buf[i].mem = &page->p_data[poffset];
        } else {
            bufv->buf[i].mem = dfs_zPage;
        }
        bufv->buf[i].size = psize;
        i++;
        pg++;
        roff += psize;
        rsize -= psize;
    }
    bufv->count = i;
}

/* Flush blockmap of the inode */
void
dfs_bmapFlush(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    assert(S_ISREG(inode->i_stat.st_mode));
    inode->i_bmapdirty = false;
}

/* Truncate pages beyond the new size of the file */
uint64_t
dfs_truncPages(struct inode *inode, off_t size) {
    uint64_t pg = size / DFS_BLOCK_SIZE;
    uint64_t i, bcount = 0, tcount = 0;
    struct page *page;
    off_t poffset;
    char *data;

    /* Copy page list before changing it */
    if (inode->i_shared) {
        if (size == 0) {
            inode->i_stat.st_blocks = 0;
            inode->i_shared = false;
            inode->i_pcache = true;
            inode->i_page = NULL;
            inode->i_pcount = 0;
            return 0;
        }
        dfs_copyPages(inode);
    }
    assert(!inode->i_shared);

    /* Remove pages past the new size */
    page = inode->i_page;
    for (i = pg; i < inode->i_pcount; i++) {
        page = &inode->i_page[i];
        if (page->p_data == NULL) {
            assert(!page->p_shared);
            continue;
        }
        if ((pg == i) && ((size % DFS_BLOCK_SIZE) != 0)) {

            /* If a page is partially truncated, keep it */
            poffset = size % DFS_BLOCK_SIZE;
            if (page->p_shared) {
                data = page->p_data;
                page->p_data = malloc(DFS_BLOCK_SIZE);
                memcpy(page->p_data, data, poffset);
                page->p_shared = false;
            }
            memset(&page->p_data[poffset], 0, DFS_BLOCK_SIZE - poffset);
        } else {
            if (page->p_shared) {
                page->p_shared = false;
            } else {
                free(page->p_data);
                tcount++;
            }
            page->p_data = NULL;
            bcount++;
        }
    }
    assert(inode->i_stat.st_blocks >= bcount);
    inode->i_stat.st_blocks -= bcount;
    if (size == 0) {
        assert(inode->i_stat.st_blocks == 0);
        if (inode->i_page) {
            free(inode->i_page);
            inode->i_page = NULL;
            inode->i_pcount = 0;
        }
        assert(inode->i_pcount == 0);
        inode->i_pcache = true;
    }
    return tcount;
}
