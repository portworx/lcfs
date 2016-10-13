#include "includes.h"

static char dfs_zPage[DFS_BLOCK_SIZE];

/* Page structure used for caching a file system block */
struct page {

    /* Page number */
    uint64_t p_page;

    /* Next page in the page chain of the inode */
    struct page *p_next;

    /* Previous page in the page chain of the inode */
    struct page *p_prev;

    /* Data associated with page of the file */
    char *p_data;

    /* Page is shared with other inodes in the snapshot chain */
    bool p_shared;
};

/* Find the requested page in the inode chain */
static struct page *
dfs_findPage(struct inode *inode, uint64_t pg) {
    uint64_t mid = (inode->i_stat.st_size / DFS_BLOCK_SIZE) / 2;
    bool reverse = (pg < mid);
    struct page *page;

    page = reverse ? inode->i_lpage : inode->i_page;
    while (page) {
        if (pg == page->p_page) {
            return page;
        }
        page = reverse ? page->p_prev : page->p_next;
    }
    return NULL;
}

/* Create a new page chain for the inode, copying existing page structures */
static void
dfs_copyPages(struct inode *inode) {
    struct page *page, *opage;
    uint64_t count = 0;

    opage = inode->i_lpage;
    inode->i_page = NULL;
    inode->i_lpage = NULL;
    while (opage) {
        page = malloc(sizeof(struct page));
        page->p_page = opage->p_page;
        page->p_data = opage->p_data;
        page->p_shared = true;
        page->p_next = inode->i_page;
        page->p_prev = NULL;
        if (inode->i_page) {
            inode->i_page->p_prev = page;
        }
        inode->i_page = page;
        if (inode->i_lpage == NULL) {
            inode->i_lpage = page;
        }
        opage = opage->p_prev;
        count++;
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

/* Add or update existing page of the inode with new data provided */
static int
dfs_addPage(struct inode *inode, uint64_t pg, off_t poffset, size_t psize,
            struct fuse_bufvec *bufv) {
    struct page *page;
    bool newblock;
    char *data;

    /* Check if a page already exists */
    if ((pg * DFS_BLOCK_SIZE) < inode->i_stat.st_size) {
        page = dfs_findPage(inode, pg);
    } else {
        page = NULL;
    }
    if (page) {

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
    page = malloc(sizeof(struct page));
    page->p_page = pg;
    page->p_shared = false;
    page->p_data = malloc(DFS_BLOCK_SIZE);
    if (poffset != 0) {
        memset(page->p_data, 0, poffset);
    }
    if ((poffset + psize) != DFS_BLOCK_SIZE) {
        memset(&page->p_data[(poffset + psize)], 0,
               DFS_BLOCK_SIZE - (poffset + psize));
    }
    dfs_updateVec(page, bufv, poffset, psize);
    page->p_next = inode->i_page;
    page->p_prev = NULL;
    if (inode->i_page) {
        inode->i_page->p_prev = page;
    }
    inode->i_page = page;
    if (inode->i_lpage == NULL) {
        inode->i_lpage = page;
    }
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
    assert(!inode->i_shared);
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

        /* Check if a page exists for the file or not */
        if (page && page->p_prev && (page->p_prev->p_page == pg)) {
            page = page->p_prev;
        } else if (page && page->p_next && (page->p_next->p_page == pg)) {
            page = page->p_next;
        } else {
            page = dfs_findPage(inode, pg);
        }
        if (page) {
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

/* Truncate pages beyond the new size of the file */
uint64_t
dfs_truncPages(struct inode *inode, off_t size) {
    uint64_t pg = size / DFS_BLOCK_SIZE;
    struct page *page, *opage = NULL;
    int bcount = 0, tcount = 0;
    char *data;

    /* Copy page list before changing it */
    if (inode->i_shared) {
        if (size == 0) {
            inode->i_stat.st_blocks = 0;
            inode->i_page = NULL;
            inode->i_lpage = NULL;
            inode->i_shared = false;
            return 0;
        }
        dfs_copyPages(inode);
    }
    assert(!inode->i_shared);

    /* Remove pages past the new size */
    page = inode->i_page;
    while (page) {
        if ((pg == page->p_page) && ((size % DFS_BLOCK_SIZE) != 0)) {

            /* If a page is partially truncated, keep it */
            if (page->p_shared) {
                data = page->p_data;
                page->p_data = malloc(DFS_BLOCK_SIZE);
                memcpy(page->p_data, data, DFS_BLOCK_SIZE);
                page->p_shared = false;
            }
            opage = page;
            page = page->p_next;
        } else if (pg <= page->p_page) {

            /* Take out this page */
            if (page->p_next) {
                page->p_next->p_prev = opage;
            }
            if (opage) {
                opage->p_next = page->p_next;
            } else {
                inode->i_page = page->p_next;
            }
            if (!page->p_shared) {
                free(page->p_data);
                tcount++;
            }
            free(page);
            bcount++;
            page = opage ? opage->p_next : inode->i_page;
        } else {
            opage = page;
            page = page->p_next;
        }
    }
    inode->i_lpage = opage;
    assert(inode->i_lpage || (size == 0));
    assert((inode->i_page == NULL) || (size != 0));
    assert(inode->i_stat.st_blocks >= bcount);
    inode->i_stat.st_blocks -= bcount;
    assert((inode->i_stat.st_blocks == 0) || (size != 0));
    return tcount;
}
