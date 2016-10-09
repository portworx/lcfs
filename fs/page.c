#include "includes.h"

/* Page structure used for caching a file system block */
struct page {

    /* Page number */
    uint64_t p_page;

    /* Next page in the page chain of the inode */
    struct page *p_next;

    /* Data associated with page of the file */
    char *p_data;

    /* Page is shared with other inodes in the snapshot chain */
    bool p_shared;
};

/* Find the requested page in the inode chain */
static struct page *
dfs_findPage(struct inode *inode, uint64_t pg) {
    struct page *page = inode->i_page;

    while (page) {
        if (pg == page->p_page) {
            return page;
        }
        page = page->p_next;
    }
    return NULL;
}

/* Create a new page chain for the inode, copying existing page structures */
static void
dfs_copyPages(struct inode *inode) {
    struct page *page,*opage;

    opage = inode->i_page;
    inode->i_page = NULL;
    while (opage) {
        page = malloc(sizeof(struct page));
        page->p_page = opage->p_page;
        page->p_data = opage->p_data;
        page->p_shared = true;
        page->p_next = inode->i_page;
        inode->i_page = page;
        opage = opage->p_next;
    }
    inode->i_shared = false;
}

/* Add or update existing page of the inode with new data provided */
void
dfs_addPage(struct inode *inode, uint64_t pg, off_t poffset, size_t psize,
            const char *buf) {
    struct page *page;
    char *data;

    assert(S_ISREG(inode->i_stat.st_mode));

    /* Copy page headers if page chain is shared */
    if (inode->i_shared) {
        dfs_copyPages(inode);
    }
    assert(!inode->i_shared);

    /* Check if a page already exists */
    page = dfs_findPage(inode, pg);
    if (page) {

        /* Allocate a new page if current page is shared */
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
        memcpy(&page->p_data[poffset], buf, psize);
        return;
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
    memcpy(&page->p_data[poffset], buf, psize);
    page->p_next = inode->i_page;
    inode->i_page = page;
}

/* Read specified pages of a file */
void
dfs_readPages(struct inode *inode, off_t soffset, off_t endoffset, char *buf) {
    uint64_t pg = soffset / DFS_BLOCK_SIZE;
    off_t poffset, off = soffset, roff = 0;
    size_t psize, rsize = endoffset - off;
    struct page *page;

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
        page = dfs_findPage(inode, pg);
        if (page) {
            memcpy(&buf[roff], &page->p_data[poffset], psize);
        } else {
            memset(&buf[roff], 0, psize);
        }
        pg++;
        roff += psize;
        rsize -= psize;
    }
}

/* Truncate pages beyond the new size of the file */
void
dfs_truncPages(struct inode *inode, off_t size) {
    uint64_t pg = size / DFS_BLOCK_SIZE;
    struct page *page, *opage = NULL;
    char *data;

    /* Copy page list before changing it */
    if (inode->i_shared) {
        if (size == 0) {
            inode->i_page = NULL;
            inode->i_shared = false;
            return;
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
            if (opage) {
                opage->p_next = page->p_next;
            } else {
                inode->i_page = page->p_next;
            }
            if (!page->p_shared) {
                free(page->p_data);
            }
            free(page);
            page = opage ? opage->p_next : inode->i_page;
        } else {
            opage = page;
            page = page->p_next;
        }
    }
    assert((inode->i_page == NULL) || (size != 0));
}
