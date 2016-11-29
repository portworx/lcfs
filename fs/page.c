#include "includes.h"

static char lc_zPage[LC_BLOCK_SIZE];

/* Return the requested page if allocated already */
static inline struct dpage *
lc_findDirtyPage(struct inode *inode, uint64_t pg) {
    return (pg < inode->i_pcount) ? &inode->i_page[pg] : NULL;
}

/* Fill up a partial page */
static void
lc_fillPage(struct inode *inode, struct dpage *dpage, uint64_t pg) {
    uint16_t poffset, psize, dsize, eof;
    struct page *bpage;
    char *data, *pdata;
    uint64_t block;

    poffset = dpage->dp_poffset;
    psize = dpage->dp_psize;
    pdata = dpage->dp_data;

    /* If the page is written partially, check if a block exists for the page.
     * If there is one read that in.
     */
    if ((poffset != 0) ||
        (((pg * LC_BLOCK_SIZE) + psize) < inode->i_stat.st_size)) {
        block = lc_inodeBmapLookup(inode, pg);
        if (block != LC_PAGE_HOLE) {
            bpage = lc_getPage(inode->i_fs, block, true);
            data = bpage->p_data;
        } else {
            data = NULL;
        }
    } else {
        data = NULL;
    }

    /* Copy partial data or zero out partial page if page is new */
    if (poffset != 0) {
        if (data) {
            memcpy(pdata, data, poffset);
        } else {
            memset(pdata, 0, poffset);
        }
        dpage->dp_poffset = 0;
    }
    if ((poffset + psize) != LC_BLOCK_SIZE) {
        dsize = LC_BLOCK_SIZE - (poffset + psize);
        if (data) {
            eof = (pg == (inode->i_stat.st_size / LC_BLOCK_SIZE)) ?
                  (inode->i_stat.st_size % LC_BLOCK_SIZE) : 0;
            if (eof) {
                assert(eof >= (poffset + psize));
                dsize = eof - (poffset + psize);
                memset(&pdata[eof], 0, LC_BLOCK_SIZE - eof);
            }
            if (dsize) {
                memcpy(&pdata[(poffset + psize)], &data[(poffset + psize)],
                       dsize);
            }
        } else {
            memset(&pdata[(poffset + psize)], 0, dsize);
        }
        dpage->dp_psize = LC_BLOCK_SIZE;
    }
    if (data) {
        lc_releasePage(inode->i_fs->fs_gfs, inode->i_fs, bpage, true);
    }
}

/* Remove a dirty page from the inode's list */
static inline char *
lc_removeDirtyPage(struct inode *inode, uint64_t pg, bool release) {
    struct dpage *page;
    char *pdata;

    assert(pg < inode->i_pcount);
    page = &inode->i_page[pg];
    pdata = page->dp_data;
    if (pdata) {
        if (release) {
            free(pdata);
        } else if ((page->dp_poffset != 0) ||
                   (page->dp_psize != LC_BLOCK_SIZE)) {

            /* Fill up a partial page before returning */
            lc_fillPage(inode, page, pg);
        }
        page->dp_data = NULL;
    }
    return release ? NULL : pdata;
}

/* Add the page to bufvec */
static inline void
lc_updateVec(char *pdata, struct fuse_bufvec *bufv,
              off_t poffset, size_t psize) {
    bufv->buf[bufv->count].mem = &pdata[poffset];
    bufv->buf[bufv->count].size = psize;
    bufv->count++;
}

/* Allocate/extend inode page table */
static void
lc_inodeAllocPages(struct inode *inode) {
    uint64_t lpage, count, size, tsize;
    struct dpage *page;

    assert(!inode->i_shared);
    lpage = (inode->i_stat.st_size + LC_BLOCK_SIZE - 1) / LC_BLOCK_SIZE;
    if (inode->i_pcount <= lpage) {

        /* Double the size of the list everytime inode is grown beyond the size
         * of the list.
         */
        count = inode->i_pcount ? (inode->i_pcount * 2) :
                                  (lpage ? (lpage + 1) : LC_PAGECACHE_SIZE);
        while (count <= lpage) {
            count *= 2;
        }
        tsize = count * sizeof(struct dpage);
        page = malloc(tsize);
        if (inode->i_pcount) {
            size = inode->i_pcount * sizeof(struct dpage);
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

/* Get a dirty page and filled up with valid data */
static char *
lc_getDirtyPage(struct inode *inode, uint64_t pg) {
    struct dpage *dpage;
    char *pdata;

    dpage = lc_findDirtyPage(inode, pg);
    pdata = dpage ? dpage->dp_data : NULL;
    if (pdata &&
        ((dpage->dp_poffset != 0) || (dpage->dp_psize != LC_BLOCK_SIZE))) {
        lc_fillPage(inode, dpage, pg);
    }
    return pdata;
}

/* Add or update existing page of the inode with new data provided */
static uint64_t
lc_mergePage(struct inode *inode, uint64_t pg, char *data,
             uint16_t poffset, uint16_t psize) {
    uint16_t doff, dsize;
    struct dpage *dpage;
    bool fill;

    assert(poffset >= 0);
    assert(poffset < LC_BLOCK_SIZE);
    assert(psize > 0);
    assert(psize <= LC_BLOCK_SIZE);
    assert(!inode->i_shared);
    assert(pg < inode->i_pcount);

    dpage = lc_findDirtyPage(inode, pg);

    /* If no dirty page exists, add the new page and return */
    if (dpage->dp_data == NULL) {
        dpage->dp_data = data;
        dpage->dp_poffset = poffset;
        dpage->dp_psize = psize;
        return 1;
    }

    /* If the current dirty page is partial page and this new write is not a
     * contiguos write, initialize exisitng page correctly and copy new data.
     */
    if (((dpage->dp_poffset != 0) || (dpage->dp_psize != LC_BLOCK_SIZE)) &&
        ((poffset != dpage->dp_poffset) ||
         ((poffset + psize) != (dpage->dp_poffset + dpage->dp_psize)))) {
        fill = false;
        dsize = 0;
        if (poffset < dpage->dp_poffset) {
            if ((poffset + psize) < dpage->dp_poffset) {
                fill = true;
                doff = 0;
            } else {
                doff = poffset;
                dsize += (dpage->dp_poffset - poffset);
            }
        } else {
            doff = dpage->dp_poffset;
        }
        if (!fill &&
            ((poffset + psize) > (dpage->dp_poffset + dpage->dp_psize))) {
            if (poffset > (dpage->dp_poffset + dpage->dp_psize)) {
                fill = true;
            } else {
                dsize += (poffset + psize) -
                         (dpage->dp_poffset + dpage->dp_psize);
            }
        }
        if (fill) {
            lc_fillPage(inode, dpage, pg);
        } else {
            dpage->dp_poffset = doff;
            dpage->dp_psize += dsize;
        }
    }
    memcpy(&dpage->dp_data[poffset], &data[poffset], psize);
    free(data);
    return 0;
}

/* Copy in provided pages */
uint64_t
lc_copyPages(off_t off, size_t size, struct dpage *dpages,
             struct fuse_bufvec *bufv, struct fuse_bufvec *dst) {
    uint64_t page, spage, pcount = 0, poffset;
    size_t wsize = size, psize;
    char *pdata;

    spage = off / LC_BLOCK_SIZE;
    page = spage;

    /* Break the down the write into pages */
    while (wsize) {
        if (page == spage) {
            poffset = off % LC_BLOCK_SIZE;
            psize = LC_BLOCK_SIZE - poffset;
        } else {
            poffset = 0;
            psize = LC_BLOCK_SIZE;
        }
        if (psize > wsize) {
            psize = wsize;
        }
        malloc_aligned((void **)&pdata);
        lc_updateVec(pdata, dst, poffset, psize);
        dpages[pcount].dp_data = pdata;
        dpages[pcount].dp_poffset = poffset;
        dpages[pcount].dp_psize = psize;
        pcount++;
        page++;
        wsize -= psize;
    }
    wsize = fuse_buf_copy(dst, bufv, FUSE_BUF_SPLICE_NONBLOCK);
    assert(wsize == size);
    return pcount;
}

/* Update pages of a file with provided data */
uint64_t
lc_addPages(struct inode *inode, off_t off, size_t size,
            struct dpage *dpages, uint64_t pcount) {
    uint64_t page, spage, count = 0;
    off_t endoffset = off + size;
    struct dpage *dpage;
    uint64_t added = 0;

    assert(S_ISREG(inode->i_stat.st_mode));

    spage = off / LC_BLOCK_SIZE;
    page = spage;

    /* Update inode size if needed */
    if (endoffset > inode->i_stat.st_size) {
        inode->i_stat.st_size = endoffset;
    }

    /* Copy page headers if page chain is shared */
    if (inode->i_shared) {
        lc_copyBmap(inode);
    }
    if (inode->i_extentLength) {
        lc_expandBmap(inode);
    }
    lc_inodeAllocPages(inode);

    /* Link the dirty pages to the inode, merging with any existing ones */
    while (count < pcount) {
        dpage = &dpages[count];
        added += lc_mergePage(inode, page, dpage->dp_data, dpage->dp_poffset,
                              dpage->dp_psize);
        page++;
        count++;
    }
    return added;
}

/* Read specified pages of a file */
void
lc_readPages(fuse_req_t req, struct inode *inode, off_t soffset,
             off_t endoffset, struct page **pages, struct fuse_bufvec *bufv) {
    uint64_t block, pg = soffset / LC_BLOCK_SIZE, pcount = 0, i = 0;
    off_t poffset, off = soffset, roff = 0;
    size_t psize, rsize = endoffset - off;
    struct fs *fs = inode->i_fs;
    struct page *page = NULL;
    char *data;

    /* XXX Issue a single read if pages are not present in cache */
    assert(S_ISREG(inode->i_stat.st_mode));
    while (rsize) {
        if (off == soffset) {
            poffset = soffset % LC_BLOCK_SIZE;
            psize = LC_BLOCK_SIZE - poffset;
        } else {
            poffset = 0;
            psize = LC_BLOCK_SIZE;
        }
        if (psize > rsize) {
            psize = rsize;
        }

        /* Check if a dirty page exists */
        data = lc_getDirtyPage(inode, pg);
        if (data == NULL) {

            /* Check bmap to find the block of the page */
            block = lc_inodeBmapLookup(inode, pg);
            if (block == LC_PAGE_HOLE) {
                bufv->buf[i].mem = lc_zPage;
            } else {

                /* Get the page */
                page = lc_getPage(fs, block, true);
                bufv->buf[i].mem = &page->p_data[poffset];
                pages[pcount++] = page;
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
    fuse_reply_data(req, bufv, FUSE_BUF_SPLICE_MOVE);
    lc_releaseReadPages(fs->fs_gfs, fs, pages, pcount);
}

/* Free blocks in the extent list */
static void
lc_freeInodeDataBlocks(struct fs *fs, struct inode *inode,
                       struct extent **extents) {
    struct extent *extent = *extents, *tmp;

    while (extent) {
        lc_freeLayerDataBlocks(fs, extent->ex_start, extent->ex_count,
                               inode->i_private);
        tmp = extent;
        extent = extent->ex_next;
        free(tmp);
    }
}

/* Flush dirty pages of an inode */
void
lc_flushPages(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    uint64_t count = 0, bcount = 0, start, end = 0, fcount = 0;
    struct page *page, *dpage = NULL, *tpage = NULL;
    bool single = true, ended = false;
    uint64_t i, lpage, pcount, block;
    struct extent *extents = NULL;
    char *pdata;

    assert(S_ISREG(inode->i_stat.st_mode));
    assert(!inode->i_shared);

    /* If inode does not have any pages, return */
    if ((inode->i_page == NULL) || (inode->i_stat.st_size == 0)) {
        assert(inode->i_page == NULL);
        return;
    }
    lpage = (inode->i_stat.st_size + LC_BLOCK_SIZE - 1) / LC_BLOCK_SIZE;
    assert(lpage < inode->i_pcount);

    /* Count the dirty pages and check if whole file can be placed
     * contiguous on disk
     */
    start = lpage;
    for (i = 0; i <= lpage; i++) {
        if (inode->i_page[i].dp_data) {
            if (ended) {
                single = false;
            }
            bcount++;
            if (i < start) {
                start = i;
            }
            end = i;
        } else {
            if ((i < inode->i_extentLength) ||
                ((i < inode->i_bcount) && inode->i_bmap[i])) {
                single = false;
            }
            ended = true;
        }
    }
    assert(bcount);

    /* XXX Deal with a fragmented file system by allocating smaller chunks */
    block = lc_blockAlloc(fs, bcount, false);

    /* Check if file has a single extent */
    if (single) {

        /* Free any old blocks present */
        if (inode->i_extentLength) {
            lc_freeLayerDataBlocks(fs, inode->i_extentBlock,
                                   inode->i_extentLength, inode->i_private);
        } else if (inode->i_bmap) {
            for (i = 0; i < inode->i_bcount; i++) {
                if (inode->i_bmap[i]) {
                    lc_addExtent(gfs, &extents, inode->i_bmap[i], 1);
                }
            }
            free(inode->i_bmap);
            inode->i_bmap = NULL;
            inode->i_bcount = 0;
        }
        inode->i_extentBlock = block;
        inode->i_extentLength = bcount;
    } else {
        if (inode->i_extentLength) {
            lc_expandBmap(inode);
        }
        lc_inodeBmapAlloc(inode);
    }

    /* Queue the dirty pages for flushing after associating with newly
     * allocated blocks
     */
    for (i = start; i <= end; i++) {
        pdata = lc_removeDirtyPage(inode, i, false);
        if (pdata) {
            page = lc_getPageNew(gfs, fs, block + count, pdata);
            if (tpage == NULL) {
                tpage = page;
            }
            page->p_dnext = dpage;
            dpage = page;
            if (!single) {
                if (inode->i_bmap[i]) {
                    lc_addExtent(gfs, &extents, inode->i_bmap[i], 1);
                }
                lc_inodeBmapAdd(inode, i, block + count);
            }
            count++;
            fcount++;

            /* Issue write after accumulating certain number of pages,
             * otherwise queue the page for later flushing.
             */
            if (fs->fs_dpcount &&
                ((fcount + fs->fs_dpcount) >= LC_CLUSTER_SIZE)) {
                lc_addPageForWriteBack(gfs, fs, dpage, tpage, fcount);
                dpage = NULL;
                tpage = NULL;
                fcount = 0;
            } else if (fcount >= LC_CLUSTER_SIZE) {
                if (fs->fs_dpcount) {
                    lc_flushDirtyPages(gfs, fs);
                }
                lc_flushPageCluster(gfs, fs, page, fcount);
                dpage = NULL;
                tpage = NULL;
                fcount = 0;
            }
        }
    }
    if (fcount) {
        lc_addPageForWriteBack(gfs, fs, dpage, tpage, fcount);
    }
    assert(bcount == count);

    /* Free dirty page list as all pages are in block cache */
    if (inode->i_page) {
        free(inode->i_page);
        inode->i_page = NULL;
        inode->i_pcount = 0;
    }
    if (extents) {
        lc_freeInodeDataBlocks(fs, inode, &extents);
    }
    if (count) {
        pcount = __sync_fetch_and_sub(&inode->i_fs->fs_pcount, count);
        assert(pcount >= count);
    }
}

/* Truncate pages beyond the new size of the file */
void
lc_truncPages(struct inode *inode, off_t size, bool remove) {
    uint64_t pg = size / LC_BLOCK_SIZE, lpage;
    struct extent *extents = NULL;
    uint64_t i, bcount = 0, pcount;
    struct fs *fs = inode->i_fs;
    bool truncated = false;
    struct dpage *dpage;
    struct gfs *gfs;
    int freed = 0;
    off_t poffset;
    char *pdata;

    /* If nothing to truncate, return */
    if ((inode->i_bmap == NULL) && (inode->i_pcount == 0) &&
        (inode->i_extentLength == 0)) {
        assert(inode->i_stat.st_blocks == 0);
        assert(inode->i_stat.st_size == 0);
        assert(inode->i_bcount == 0);
        assert(inode->i_pcount == 0);
        assert(inode->i_page == NULL);
        assert(!inode->i_shared);
        inode->i_private = true;
        return;
    }

    /* Copy bmap list before changing it */
    if (inode->i_shared) {
        if (size == 0) {
            if (remove) {
                inode->i_stat.st_blocks = 0;
                inode->i_shared = false;
                inode->i_private = true;
                inode->i_extentBlock = 0;
                inode->i_extentLength = 0;
            }
            inode->i_page = NULL;
            inode->i_pcount = 0;
            inode->i_bcount = 0;
            inode->i_bmap = NULL;
            return;
        }
        lc_copyBmap(inode);
    }
    assert(!inode->i_shared);
    gfs = fs->fs_gfs;

    /* Take care of files with single extent */
    lpage = (inode->i_stat.st_size + LC_BLOCK_SIZE - 1) / LC_BLOCK_SIZE;
    if (remove && inode->i_extentLength) {
        assert(inode->i_bcount == 0);
        assert(inode->i_pcount == 0);
        if (size % LC_BLOCK_SIZE) {
            lc_expandBmap(inode);
        } else {
            if (inode->i_extentLength > pg) {
                bcount = inode->i_extentLength - pg;
                lc_addExtent(gfs, &extents,
                             inode->i_extentBlock + pg, bcount);
                inode->i_extentLength = pg;
            }
            if (inode->i_extentLength == 0) {
                inode->i_extentBlock = 0;
            }
        }
    }

    /* Remove blockmap entries past the new size */
    if (remove && inode->i_bcount) {
        assert(inode->i_stat.st_blocks <= inode->i_bcount);
        for (i = pg; i < inode->i_bcount; i++) {
            if (inode->i_bmap[i] == 0) {
                continue;
            }
            if ((pg == i) && ((size % LC_BLOCK_SIZE) != 0)) {

                /* If a page is partially truncated, keep it */
                poffset = size % LC_BLOCK_SIZE;
                lc_inodeAllocPages(inode);
                dpage = lc_findDirtyPage(inode, pg);
                if (dpage->dp_data == NULL) {
                    malloc_aligned((void **)&dpage->dp_data);
                    __sync_add_and_fetch(&fs->fs_pcount, 1);
                    dpage->dp_poffset = 0;
                    dpage->dp_psize = 0;
                } else {
                    if ((dpage->dp_poffset + dpage->dp_psize) > poffset) {
                        if (dpage->dp_poffset >= poffset) {
                            dpage->dp_poffset = 0;
                            dpage->dp_psize = 0;
                        } else {
                            dpage->dp_psize = poffset - dpage->dp_poffset;
                        }
                    }
                }
                truncated = true;
            } else {
                lc_addExtent(gfs, &extents, inode->i_bmap[i], 1);
                inode->i_bmap[i] = 0;
                bcount++;
            }
        }
    }

    /* Remove dirty pages past the new size from the dirty list */
    if (inode->i_pcount) {
        assert(lpage < inode->i_pcount);
        for (i = pg; i <= lpage; i++) {
            dpage = lc_findDirtyPage(inode, i);
            pdata = dpage->dp_data;
            if (pdata == NULL) {
                continue;
            }
            if ((pg == i) && ((size % LC_BLOCK_SIZE) != 0)) {

                /* If a page is partially truncated, keep it */
                if (!truncated) {
                    poffset = size % LC_BLOCK_SIZE;
                    if ((dpage->dp_poffset + dpage->dp_psize) > poffset) {
                        if (dpage->dp_poffset >= poffset) {
                            dpage->dp_poffset = 0;
                            dpage->dp_psize = 0;
                        } else {
                            dpage->dp_psize = poffset - dpage->dp_poffset;
                        }
                    }
                }
            } else {
                lc_removeDirtyPage(inode, i, true);
                freed++;
            }
        }
    }
    if (extents) {
        lc_freeInodeDataBlocks(fs, inode, &extents);
    }
    if (freed) {
        pcount = __sync_fetch_and_sub(&fs->fs_pcount, freed);
        assert(pcount >= freed);
    }

    /* Update inode blocks while truncating the file */
    if (remove) {
        assert(inode->i_stat.st_blocks >= bcount);
        inode->i_stat.st_blocks -= bcount;
    }

    /* If the file is fully truncated, free bmap and page lists */
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
        inode->i_private = true;
    }
}
