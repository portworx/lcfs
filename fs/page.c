#include "includes.h"

static char dfs_zPage[DFS_BLOCK_SIZE];

#define DFS_PAGECACHE_SIZE  1
#define DFS_CLUSTER_SIZE    256

/* Return the hash number for the block number provided */
static inline int
dfs_pageBlockHash(uint64_t block) {
    return block % DFS_PCACHE_SIZE;
}

/* Allocate and initialize page block hash table */
struct pcache *
dfs_pcache_init() {
    struct pcache *pcache = malloc(sizeof(struct pcache) * DFS_PCACHE_SIZE);
    int i;

    for (i = 0; i < DFS_PCACHE_SIZE; i++) {
        pthread_mutex_init(&pcache[i].pc_lock, NULL);
        pcache[i].pc_head = NULL;
    }
    pcache->pc_pcount = 0;
    return pcache;
}

/* Insert a page into page hash */
static void
dfs_insertPage(struct fs *fs, uint64_t block, char *data) {
    struct pcache *pcache = fs->fs_pcache;
    int hash = dfs_pageBlockHash(block);
    struct page *hpage;

    hpage = malloc(sizeof(struct page));
    hpage->p_data = data;
    hpage->p_block = block;
    pthread_rwlock_init(&hpage->p_lock, NULL);
    pthread_mutex_lock(&pcache[hash].pc_lock);
    hpage->p_cnext = pcache[hash].pc_head;
    pcache[hash].pc_head = hpage;
    pthread_mutex_unlock(&pcache[hash].pc_lock);
}

/* Lookup a page in the block hash */
static struct page *
dfs_lookupPage(struct fs *fs, uint64_t block) {
    int hash = dfs_pageBlockHash(block);
    struct pcache *pcache = fs->fs_pcache;
    struct page *page;
    bool incr;

    assert(block);
    assert(block != DFS_PAGE_HOLE);
    pthread_mutex_lock(&pcache[hash].pc_lock);
    page = pcache[hash].pc_head;
    while (page) {
        if (page->p_block == block) {
            break;
        }
        page = page->p_cnext;
    }
    if (page == NULL) {
        page = malloc(sizeof(struct page));
        page->p_data = NULL;
        page->p_block = block;
        pthread_rwlock_init(&page->p_lock, NULL);
        page->p_cnext = pcache[hash].pc_head;
        pcache[hash].pc_head = page;
    }
    pthread_mutex_unlock(&pcache[hash].pc_lock);
    if (page->p_data == NULL) {
        incr = false;
        pthread_rwlock_wrlock(&page->p_lock);
        if (page->p_data == NULL) {
            page->p_data = dfs_readBlock(fs->fs_gfs->gfs_fd, block);
            incr = true;
        }
        pthread_rwlock_unlock(&page->p_lock);
        if (incr) {
            __sync_add_and_fetch(&pcache->pc_pcount, 1);
        }
    }
    return page;
}

/* Remove pages from page cache and free the hash table */
void
dfs_destroy_pages(struct pcache *pcache) {
    uint64_t count = 0;
    struct page *page;
    int i;

    for (i = 0; i < DFS_PCACHE_SIZE; i++) {
        while ((page = pcache[i].pc_head)) {
            pcache[i].pc_head = page->p_cnext;
            page->p_block = DFS_INVALID_BLOCK;
            page->p_cnext = NULL;
            pthread_rwlock_destroy(&page->p_lock);
            if (page->p_data) {
                free(page->p_data);
                count++;
            }
            free(page);
        }
        assert(pcache[i].pc_head == NULL);
        pthread_mutex_destroy(&pcache[i].pc_lock);
    }
    if (count) {
        __sync_sub_and_fetch(&pcache->pc_pcount, count);
    }
    assert(pcache->pc_pcount == 0);
    free(pcache);
}

/* Add a new dirty page */
static inline void
dfs_insertDirtyPage(struct inode *inode, uint64_t pg, char *pdata) {
    assert(pg < inode->i_pcount);
    assert(inode->i_page[pg] == NULL);
    inode->i_page[pg] = pdata;
}

/* Return the requested page if allocated already */
static inline char *
dfs_findDirtyPage(struct inode *inode, uint64_t pg) {
    return (pg < inode->i_pcount) ? inode->i_page[pg] : NULL;
}

/* Remove a dirty page */
static inline void
dfs_removeDirtyPage(struct inode *inode, uint64_t pg) {
    char *pdata = inode->i_page[pg];

    free(pdata);
    inode->i_page[pg] = NULL;
}

/* Allocate a bmap list for the inode */
static void
dfs_inodeBmapAlloc(struct inode *inode) {
    uint64_t lpage, count, size, tsize;
    uint64_t *blocks;

    lpage = (inode->i_stat.st_size + DFS_BLOCK_SIZE - 1) / DFS_BLOCK_SIZE;
    if (inode->i_bcount <= lpage) {
        count = inode->i_bcount ? (inode->i_bcount * 2) :
                                  (lpage ? (lpage + 1) : DFS_PAGECACHE_SIZE);
        while (count <= lpage) {
            count *= 2;
        }
        tsize = count * sizeof(uint64_t);
        blocks = malloc(tsize);
        if (inode->i_bcount) {
            size = inode->i_bcount * sizeof(uint64_t);
            memcpy(blocks, inode->i_bmap, size);
            memset(&blocks[inode->i_bcount], 0, tsize - size);
            free(inode->i_bmap);
        } else {
            assert(inode->i_bmap == NULL);
            memset(blocks, 0, tsize);
        }
        inode->i_bcount = count;
        inode->i_bmap = blocks;
    }
}

/* Add a bmap entry to the inode */
static void
dfs_inodeBmapAdd(struct inode *inode, uint64_t page, uint64_t block) {
    assert(!inode->i_shared);
    assert(inode->i_extentLength == 0);
    assert(page < inode->i_bcount);
    inode->i_bmap[page] = block;
}

/* Lookup inode bmap for the specified page */
static uint64_t
dfs_inodeBmapLookup(struct inode *inode, uint64_t page) {
    if (inode->i_extentLength && (page < inode->i_extentLength)) {
        return inode->i_extentBlock + page;
    }
    if ((page < inode->i_bcount) && inode->i_bmap[page]) {
        return inode->i_bmap[page];
    }
    return DFS_PAGE_HOLE;
}

/* Expand a single extent to a bmap list */
static void
dfs_expandBmap(struct inode *inode) {
    uint64_t i;

    inode->i_bmap = malloc(inode->i_extentLength * sizeof(uint64_t));
    for (i = 0; i < inode->i_extentLength; i++) {
        inode->i_bmap[i] = inode->i_extentBlock + i;
    }
    inode->i_bcount = inode->i_extentLength;
    inode->i_extentBlock = 0;
    inode->i_extentLength = 0;
    assert(inode->i_stat.st_blocks == inode->i_bcount);
    inode->i_bmapdirty = true;
}

/* Create a new page chain for the inode, copying existing page structures */
static void
dfs_copyBmap(struct inode *inode) {
    uint64_t *bmap;
    uint64_t size;

    if (inode->i_extentLength) {
        dfs_expandBmap(inode);
    } else {
        bmap = inode->i_bmap;
        size = inode->i_bcount * sizeof(uint64_t);
        assert(inode->i_stat.st_blocks <= inode->i_bcount);
        inode->i_bmap = malloc(size);
        memcpy(inode->i_bmap, bmap, size);
    }
    inode->i_shared = false;
}

/* Add the page to bufvec */
static inline void
dfs_updateVec(char *pdata, struct fuse_bufvec *bufv,
              off_t poffset, size_t psize) {
    bufv->buf[bufv->count].mem = &pdata[poffset];
    bufv->buf[bufv->count].size = psize;
    bufv->count++;
}

/* Allocate/extend inode page table */
static void
dfs_inodeAllocPages(struct inode *inode) {
    uint64_t lpage, count, size, tsize;
    char **page;

    assert(!inode->i_shared);
    lpage = (inode->i_stat.st_size + DFS_BLOCK_SIZE - 1) / DFS_BLOCK_SIZE;
    if (inode->i_pcount <= lpage) {
        count = inode->i_pcount ? (inode->i_pcount * 2) :
                                  (lpage ? (lpage + 1) : DFS_PAGECACHE_SIZE);
        while (count <= lpage) {
            count *= 2;
        }
        tsize = count * sizeof(char *);
        page = malloc(tsize);
        if (inode->i_pcount) {
            size = inode->i_pcount * sizeof(char *);
            memcpy(page, inode->i_page, size);
            memset(&page[inode->i_pcount], 0, tsize - size);
            free(inode->i_page);
        } else {
            assert(inode->i_page == NULL);
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
            int *incr, struct fuse_bufvec *bufv) {
    struct page *bpage;
    char *data, *pdata;
    bool new = false;
    uint64_t block;

    assert(pg < inode->i_pcount);
    assert(!inode->i_shared);
    pdata = dfs_findDirtyPage(inode, pg);
    if (pdata == NULL) {
        if ((poffset != 0) || ((poffset + psize) != DFS_BLOCK_SIZE)) {
            block = dfs_inodeBmapLookup(inode, pg);
            if (block != DFS_PAGE_HOLE) {
                bpage = dfs_lookupPage(inode->i_fs, block);
                data = bpage->p_data;
            } else {
                data = NULL;
            }
        } else {
            data = NULL;
        }
        posix_memalign((void **)&pdata, DFS_BLOCK_SIZE, DFS_BLOCK_SIZE);
        *incr = (*incr) + 1;
        if (poffset != 0) {
            if (data) {
                memcpy(pdata, data, poffset);
            } else {
                memset(pdata, 0, poffset);
            }
        }
        if ((poffset + psize) != DFS_BLOCK_SIZE) {
            if (data) {
                memcpy(&pdata[(poffset + psize)], data,
                    DFS_BLOCK_SIZE - (poffset + psize));
            } else {
                memset(&pdata[(poffset + psize)], 0,
                    DFS_BLOCK_SIZE - (poffset + psize));
            }
        }
        dfs_insertDirtyPage(inode, pg, pdata);
    }
    dfs_updateVec(pdata, bufv, poffset, psize);
    return new;
}

/* Update pages of a file with provided data */
int
dfs_addPages(struct inode *inode, off_t off, size_t size,
             struct fuse_bufvec *bufv, struct fuse_bufvec *dst) {
    size_t wsize = size, psize;
    int count = 0, added = 0;
    uint64_t page, spage;
    off_t poffset;

    assert(S_ISREG(inode->i_stat.st_mode));

    /* Copy page headers if page chain is shared */
    if (inode->i_shared) {
        dfs_copyBmap(inode);
    }
    if (inode->i_extentLength) {
        dfs_expandBmap(inode);
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
        count += dfs_addPage(inode, page, poffset, psize, &added, dst);
        page++;
        wsize -= psize;
    }
    wsize = fuse_buf_copy(dst, bufv, FUSE_BUF_SPLICE_NONBLOCK);
    assert(wsize == size);
    if (added) {
        __sync_add_and_fetch(&inode->i_fs->fs_pcount, added);
    }
    inode->i_blks += count;
    return count;
}

/* Read specified pages of a file */
void
dfs_readPages(struct inode *inode, off_t soffset, off_t endoffset,
              struct fuse_bufvec *bufv) {
    uint64_t block, pg = soffset / DFS_BLOCK_SIZE;
    off_t poffset, off = soffset, roff = 0;
    size_t psize, rsize = endoffset - off;
    struct fs *fs = inode->i_fs;
    struct page *page = NULL;
    char *data;
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

        data = dfs_findDirtyPage(inode, pg);
        if (data == NULL) {
            block = dfs_inodeBmapLookup(inode, pg);
            if (block == DFS_PAGE_HOLE) {
                bufv->buf[i].mem = dfs_zPage;
            } else {
                page = dfs_lookupPage(fs, block);
                bufv->buf[i].mem = &page->p_data[poffset];
            }
        } else {
            bufv->buf[i].mem = &data[poffset];
        }
        bufv->buf[i].size = psize;
        i++;
        pg++;
        roff += psize;
        rsize -= psize;
    }
    bufv->count = i;
}

/* Flush a cluster of pages */
static int
dfs_flushPageCluster(struct gfs *gfs, struct fs *fs,
                     char **pdata, int count, uint64_t block) {
    struct iovec *iovec;
    int i;

    //dfs_printf("Flushing %d pages to block %ld\n", count, block);
    if (count == 1) {
        dfs_writeBlock(gfs->gfs_fd, pdata[0], block);
    } else {
        iovec = alloca(count * sizeof(struct iovec));
        for (i = 0; i < count; i++) {
            iovec[i].iov_base = pdata[i];
            iovec[i].iov_len = DFS_BLOCK_SIZE;
        }
        dfs_writeBlocks(gfs->gfs_fd, iovec, count, block);
    }
    return count;
}

/* Flush dirty pages of an inode */
bool
dfs_flushPages(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    uint64_t count = 0, max, freed = 0, bcount = 0, start, end = 0;
    bool single = true, ended = false;
    uint64_t i, lpage, pcount, block;
    char *page, **pages;

    assert(S_ISREG(inode->i_stat.st_mode));
    //dfs_printf("Flushing pages of inode %ld\n", inode->i_stat.st_ino);
    if (inode->i_shared || (inode->i_page == NULL) ||
        (inode->i_stat.st_size == 0)) {
        return true;
    }
    lpage = (inode->i_stat.st_size + DFS_BLOCK_SIZE - 1) / DFS_BLOCK_SIZE;
    max = lpage;
    if (max > DFS_CLUSTER_SIZE) {
        max = DFS_CLUSTER_SIZE;
    }
    assert(lpage < inode->i_pcount);
    pages = alloca(max * sizeof(struct page *));

    /* Count the dirty pages */
    start = lpage;
    for (i = 0; i <= lpage; i++) {
        if (inode->i_page[i]) {
            if (ended) {
                single = false;
            }
            bcount++;
            if (i < start) {
                start = i;
            }
            end = i;
        } else {
            ended = true;
        }
    }
    assert(bcount);
    block = dfs_blockAlloc(fs, bcount);
    dfs_inodeBmapAlloc(inode);
    __sync_add_and_fetch(&fs->fs_pcache->pc_pcount, bcount);
    for (i = start; i <= end; i++) {
        page = inode->i_page[i];
        if (page) {
            if (count >= max) {
                freed += dfs_flushPageCluster(gfs, fs, pages, count, block);
                block += count;
                bcount -= count;
                count = 0;
            }
            dfs_insertPage(fs, block + count, page);
            /* XXX free the old block */
            dfs_inodeBmapAdd(inode, i, block + count);
            pages[count++] = page;
        }
    }
    if (count) {
        freed += dfs_flushPageCluster(gfs, fs, pages, count, block);
        bcount -= count;
    }
    assert(bcount == 0);
    if (freed) {
        pcount = __sync_fetch_and_sub(&inode->i_fs->fs_pcount, freed);
        assert(pcount >= freed);
    }
    if (inode->i_page) {
        free(inode->i_page);
        inode->i_page = NULL;
        inode->i_pcount = 0;
    }
    return single;
}

/* Flush a bmap block */
static uint64_t
dfs_flushBmapBlock(struct gfs *gfs, struct fs *fs,
                   struct bmapBlock *bblock, int count) {
    uint64_t block = dfs_blockAlloc(fs, 1);

    if (count < DFS_BMAP_BLOCK) {
        memset(&bblock->bb_bmap[count], 0,
               (DFS_BMAP_BLOCK - count) * sizeof(struct bmap));
    }
    //dfs_printf("Flushing bmap block to block %ld\n", block);
    dfs_writeBlock(gfs->gfs_fd, bblock, block);
    return block;
}

/* Flush blockmap of the inode */
void
dfs_bmapFlush(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    uint64_t block = DFS_INVALID_BLOCK, start = 0;
    struct bmapBlock *bblock = NULL;
    bool single, ended = false;
    int count = DFS_BMAP_BLOCK;
    uint64_t i, bcount = 0;
    struct bmap *bmap;

    assert(S_ISREG(inode->i_stat.st_mode));
    assert(inode->i_extentLength == 0);
    if (inode->i_removed) {
        assert(inode->i_bmap == NULL);
        assert(inode->i_page == NULL);
        inode->i_bmapdirty = false;
        return;
    }
    single = dfs_flushPages(gfs, fs, inode);
    //dfs_printf("Flushing bmap of inode %ld\n", inode->i_stat.st_ino);

    /* Check if file has a single extent */
    if (single) {
        for (i = 0; i < inode->i_bcount; i++) {
            if (inode->i_bmap[i]) {
                if (ended) {
                    single = false;
                    break;
                }
                if (i == 0) {
                    start = inode->i_bmap[i];
                } else if ((start + i) != inode->i_bmap[i]) {
                    single = false;
                    break;
                }
                bcount++;
            } else {
                ended = true;
            }
        }

        if (single) {
            assert(bcount > 0);
            inode->i_extentBlock = start;
            inode->i_extentLength = bcount;
            free(inode->i_bmap);
            inode->i_bmap = NULL;
            inode->i_bcount = 0;
            block = start;
        } else {
            bcount = 0;
        }
    }
    if (!single) {
        dfs_printf("File %ld fragmented\n", inode->i_stat.st_ino);
    }
    for (i = 0; i < inode->i_bcount; i++) {
        if (inode->i_bmap[i] == 0) {
            continue;
        }
        if (count >= DFS_BMAP_BLOCK) {
            if (bblock) {
                block = dfs_flushBmapBlock(gfs, fs, bblock, count);
            } else {
                posix_memalign((void **)&bblock, DFS_BLOCK_SIZE,
                               DFS_BLOCK_SIZE);
            }
            bblock->bb_next = block;
            count = 0;
        }
        bcount++;
        bmap = &bblock->bb_bmap[count++];
        bmap->b_off = i;
        bmap->b_block = inode->i_bmap[i];
    }
    if (bblock) {
        block = dfs_flushBmapBlock(gfs, fs, bblock, count);
        free(bblock);
    }
    inode->i_stat.st_blocks = bcount;

    /* XXX Free old bmap blocks */
    inode->i_bmapDirBlock = block;
    inode->i_bmapdirty = false;
    inode->i_dirty = true;
}

/* Read bmap blocks of a file and initialize page list */
void
dfs_bmapRead(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    struct bmapBlock *bblock = NULL;
    uint64_t i, bcount = 0;
    struct bmap *bmap;
    uint64_t block;

    //dfs_printf("Reading bmap of inode %ld\n", inode->i_stat.st_ino);
    assert(S_ISREG(inode->i_stat.st_mode));
    if (inode->i_stat.st_size == 0) {
        assert(inode->i_stat.st_blocks == 0);
        return;
    }
    if (inode->i_extentLength) {
        assert(inode->i_stat.st_blocks == inode->i_extentLength);
        assert(inode->i_extentBlock != 0);
        return;
    }
    dfs_printf("Inode %ld with fragmented extents %ld\n", inode->i_stat.st_ino, inode->i_stat.st_blocks);
    dfs_inodeBmapAlloc(inode);
    block = inode->i_bmapDirBlock;
    while (block != DFS_INVALID_BLOCK) {
        //dfs_printf("Reading bmap block %ld\n", block);
        bblock = dfs_readBlock(gfs->gfs_fd, block);
        for (i = 0; i < DFS_BMAP_BLOCK; i++) {
            bmap = &bblock->bb_bmap[i];
            if (bmap->b_block == 0) {
                break;
            }
            //dfs_printf("page %ld at block %ld\n", bmap->b_off, bmap->b_block);
            dfs_inodeBmapAdd(inode, bmap->b_off, bmap->b_block);
            bcount++;
        }
        block = bblock->bb_next;
        free(bblock);
    }
    assert(inode->i_stat.st_blocks == bcount);
}

/* Truncate pages beyond the new size of the file */
uint64_t
dfs_truncPages(struct inode *inode, off_t size, bool remove) {
    uint64_t pg = size / DFS_BLOCK_SIZE, lpage;
    uint64_t i, bcount = 0, tcount = 0, pcount;
    bool truncated = false;
    struct page *bpage;
    int freed = 0;
    off_t poffset;
    char *pdata;

    if ((inode->i_bmap == NULL) && (inode->i_pcount == 0) &&
        (inode->i_extentLength == 0)) {
        assert(inode->i_stat.st_blocks == 0);
        assert(inode->i_stat.st_size == 0);
        assert(inode->i_bcount == 0);
        assert(inode->i_pcount == 0);
        assert(inode->i_page == 0);
        assert(!inode->i_shared);
        inode->i_pcache = true;
        return 0;
    }

    /* Copy page list before changing it */
    if (inode->i_shared) {
        if (size == 0) {
            inode->i_stat.st_blocks = 0;
            inode->i_shared = false;
            inode->i_pcache = true;
            inode->i_page = NULL;
            inode->i_pcount = 0;
            inode->i_bcount = 0;
            inode->i_bmap = NULL;
            inode->i_extentBlock = 0;
            inode->i_extentLength = 0;
            return 0;
        }
        dfs_copyBmap(inode);
    }
    assert(!inode->i_shared);
    lpage = (inode->i_stat.st_size + DFS_BLOCK_SIZE - 1) / DFS_BLOCK_SIZE;
    if (inode->i_extentLength) {
        assert(inode->i_bcount == 0);
        assert(inode->i_pcount == 0);
        if (size % DFS_BLOCK_SIZE) {
            dfs_expandBmap(inode);
        } else {
            if (inode->i_extentLength > pg) {
                bcount = inode->i_extentLength - pg;
                inode->i_extentLength = pg;
            }
            if (inode->i_extentLength == 0) {
                inode->i_extentBlock = 0;
            }
        }
    }

    /* Remove blockmap entries past the new size */
    if (inode->i_bcount) {
        assert(inode->i_stat.st_blocks <= inode->i_bcount);
        for (i = pg; i < inode->i_bcount; i++) {
            if (inode->i_bmap[i] == 0) {
                continue;
            }
            if ((pg == i) && ((size % DFS_BLOCK_SIZE) != 0)) {

                /* If a page is partially truncated, keep it */
                poffset = size % DFS_BLOCK_SIZE;
                dfs_inodeAllocPages(inode);
                pdata = dfs_findDirtyPage(inode, pg);
                if (pdata == NULL) {
                    posix_memalign((void **)&pdata,
                                   DFS_BLOCK_SIZE, DFS_BLOCK_SIZE);
                    __sync_add_and_fetch(&inode->i_fs->fs_pcount, 1);
                    bpage = dfs_lookupPage(inode->i_fs, inode->i_bmap[i]);
                    memcpy(pdata, bpage->p_data, poffset);
                    dfs_insertDirtyPage(inode, pg, pdata);
                }
                memset(&pdata[poffset], 0, DFS_BLOCK_SIZE - poffset);
                inode->i_blks++;
                truncated = true;
            } else {
                inode->i_bmap[i] = 0;
                bcount++;
            }
        }
    }

    /* Remove pages past the new size from the dirty list */
    if (inode->i_pcount) {
        assert(lpage < inode->i_pcount);
        for (i = pg; i <= lpage; i++) {
            pdata = inode->i_page[i];
            if (pdata == NULL) {
                continue;
            }
            if ((pg == i) && ((size % DFS_BLOCK_SIZE) != 0)) {

                /* If a page is partially truncated, keep it */
                if (!truncated) {
                    poffset = size % DFS_BLOCK_SIZE;
                    memset(&pdata[poffset], 0,
                           DFS_BLOCK_SIZE - poffset);
                }
            } else {
                dfs_removeDirtyPage(inode, i);
                freed++;
            }
        }
    }
    if (freed) {
        pcount = __sync_fetch_and_sub(&inode->i_fs->fs_pcount, freed);
        assert(pcount >= freed);
    }
    assert(inode->i_stat.st_blocks >= bcount);
    if (remove) {
        inode->i_stat.st_blocks -= bcount;
    }
    if (size == 0) {
        assert((inode->i_stat.st_blocks == 0) || !remove);
        if (inode->i_page) {
            free(inode->i_page);
            inode->i_page = NULL;
            inode->i_pcount = 0;
        }
        if (inode->i_bmap) {
            free(inode->i_bmap);
            inode->i_bmap = NULL;
            inode->i_bcount = 0;
        }
        assert(inode->i_pcount == 0);
        assert(inode->i_bcount == 0);
        inode->i_pcache = true;

        /* XXX Does not work after remount */
        tcount = inode->i_blks;
        inode->i_blks = 0;
    }
    return tcount;
}
