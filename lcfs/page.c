#include "includes.h"

static char lc_zPage[LC_BLOCK_SIZE];

/* Calculate the hash index of dirty page */
static int
lc_dpageHash(uint64_t page) {
    return page % LC_DHASH_MIN;
}

/* Add a page to dirty page hash */
static void
lc_addDirtyPage(struct fs *fs, struct dhpage **dhpage, uint64_t page,
                char *data, uint16_t poffset, uint16_t psize) {
    int hash = lc_dpageHash(page);
    struct dhpage *hpage = lc_malloc(fs, sizeof(struct dhpage),
                                     LC_MEMTYPE_HPAGE);

    hpage->dh_pg = page;
    hpage->dh_next = dhpage[hash];
    hpage->dh_page.dp_data = data;
    hpage->dh_page.dp_poffset = poffset;
    hpage->dh_page.dp_psize = psize;
    dhpage[hash] = hpage;
}

/* Return the requested page if allocated already */
static inline struct dpage *
lc_findDirtyPage(struct inode *inode, uint64_t pg) {
    struct dhpage *dhpage;
    int hash;

    if (inode->i_flags & LC_INODE_HASHED) {
        assert(inode->i_pcount == 0);
        hash = lc_dpageHash(pg);
        dhpage = inode->i_hpage[hash];
        while (dhpage) {
            if (dhpage->dh_pg == pg) {
                return &dhpage->dh_page;
            }
            dhpage = dhpage->dh_next;
        }
        return NULL;
    }
    return (pg < inode->i_pcount) ? &inode->i_page[pg] : NULL;
}

/* Flush dirty pages if the inode accumulated too many */
static void
lc_flushInodeDirtyPages(struct inode *inode, uint64_t page) {
    struct dpage *dpage;

    /* Do not trigger flush if the last page is not fully filled up for a
     * sequentially written file.
     */
    if (inode->i_extentLength || (inode->i_emap == NULL)) {
        dpage = lc_findDirtyPage(inode, page);
        if (dpage->dp_data &&
            (dpage->dp_poffset || (dpage->dp_psize != LC_BLOCK_SIZE))) {
            return;
        }
    }
    printf("Flushing pages of inode %ld\n", inode->i_ino);
    lc_flushPages(inode->i_fs->fs_gfs, inode->i_fs, inode, false);
}

/* Add inode to the file system dirty list */
void
lc_addDirtyInode(struct fs *fs, struct inode *inode) {
    assert(inode->i_dnext == NULL);
    pthread_mutex_lock(&fs->fs_dilock);
    if (fs->fs_dirtyInodesLast) {
        fs->fs_dirtyInodesLast->i_dnext = inode;
    } else {
        assert(fs->fs_dirtyInodes == NULL);
        fs->fs_dirtyInodes = inode;
    }
    fs->fs_dirtyInodesLast = inode;
    pthread_mutex_unlock(&fs->fs_dilock);
}

/* Remove an inode from the file system dirty list */
static void
lc_removeDirtyInode(struct fs *fs, struct inode *inode, struct inode *prev) {
    if (prev) {
        prev->i_dnext = inode->i_dnext;
    } else {
        assert(fs->fs_dirtyInodes == inode);
        fs->fs_dirtyInodes = inode->i_dnext;
    }
    if (fs->fs_dirtyInodesLast == inode) {
        assert(prev || (fs->fs_dirtyInodes == NULL));
        fs->fs_dirtyInodesLast = prev;
    }
}

/* Flush inodes on the dirty list */
void
lc_flushDirtyInodeList(struct fs *fs) {
    struct inode *inode, *prev = NULL, *tmp;

    if ((fs->fs_dirtyInodes == NULL) || fs->fs_removed) {
        return;
    }
    if (pthread_mutex_trylock(&fs->fs_dilock)) {
        return;
    }
    inode = fs->fs_dirtyInodes;
    while (inode && !fs->fs_removed) {
        if (inode->i_flags & LC_INODE_REMOVED) {
            lc_removeDirtyInode(fs, inode, prev);
            tmp = inode;
            inode = inode->i_dnext;
            tmp->i_dnext = NULL;
        } else if ((inode->i_ocount == 0) &&
            !pthread_rwlock_trywrlock(&inode->i_rwlock)) {
            lc_removeDirtyInode(fs, inode, prev);
            inode->i_dnext = NULL;
            if (inode->i_page == NULL) {
                pthread_rwlock_unlock(&inode->i_rwlock);
                inode = prev ? prev->i_dnext : fs->fs_dirtyInodes;
                continue;
            }
            pthread_mutex_unlock(&fs->fs_dilock);
            lc_flushInodeDirtyPages(inode,
                                    inode->i_size / LC_BLOCK_SIZE);
            if (inode->i_page) {
                pthread_rwlock_unlock(&inode->i_rwlock);
                return;
            }
            pthread_rwlock_unlock(&inode->i_rwlock);
            if (fs->fs_pcount < (LC_MAX_LAYER_DIRTYPAGES / 2)) {
                return;
            }
            prev = NULL;
            if (pthread_mutex_trylock(&fs->fs_dilock)) {
                return;
            }
            inode = fs->fs_dirtyInodes;
        } else {
            prev = inode;
            inode = inode->i_dnext;
        }
    }
    pthread_mutex_unlock(&fs->fs_dilock);
}

/* Fill up a partial page */
static void
lc_fillPage(struct gfs *gfs, struct inode *inode, struct dpage *dpage,
            uint64_t pg) {
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
        (((pg * LC_BLOCK_SIZE) + psize) < inode->i_size)) {
        block = lc_inodeEmapLookup(gfs, inode, pg);
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
            eof = (pg == (inode->i_size / LC_BLOCK_SIZE)) ?
                  (inode->i_size % LC_BLOCK_SIZE) : 0;
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
lc_removeDirtyPage(struct gfs *gfs, struct inode *inode, uint64_t pg,
                   bool release) {
    struct dhpage *dhpage = NULL, *prev;
    struct fs *fs = inode->i_fs;
    struct dpage *page;
    char *pdata;
    int hash;

    if (inode->i_flags & LC_INODE_HASHED) {
        assert(inode->i_pcount == 0);
        page = NULL;
        prev = NULL;
        hash = lc_dpageHash(pg);
        dhpage = inode->i_hpage[hash];
        while (dhpage) {
            if (dhpage->dh_pg == pg) {
                if (prev == NULL) {
                    inode->i_hpage[hash] = dhpage->dh_next;
                } else {
                    prev->dh_next = dhpage->dh_next;
                }
                page = &dhpage->dh_page;
                break;
            }
            prev = dhpage;
            dhpage = dhpage->dh_next;
        }
    } else {
        assert(pg < inode->i_pcount);
        page = &inode->i_page[pg];
    }
    pdata = page ? page->dp_data : NULL;
    if (pdata) {
        if (release) {
            lc_free(fs, pdata, LC_BLOCK_SIZE, LC_MEMTYPE_DATA);
        } else if ((page->dp_poffset != 0) ||
                   (page->dp_psize != LC_BLOCK_SIZE)) {

            /* Fill up a partial page before returning */
            lc_fillPage(gfs, inode, page, pg);
        }
        page->dp_data = NULL;
        assert(inode->i_dpcount > 0);
        inode->i_dpcount--;
    }
    if (dhpage) {
        lc_free(fs, dhpage, sizeof(struct dhpage), LC_MEMTYPE_HPAGE);
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
    struct fs *fs = inode->i_fs;
    struct dhpage **dhpage;
    struct dpage *page;
    int i;

    assert(!(inode->i_flags & LC_INODE_SHARED));
    if (inode->i_flags & LC_INODE_HASHED) {
        assert(inode->i_pcount == 0);
        return;
    }

    /* Adapt to a hashing scheme when file grows bigger than a certain size.
     * This is not done for temporary files.
     */
    if ((inode->i_size > (LC_DHASH_MIN * LC_BLOCK_SIZE)) &&
        !(inode->i_flags & LC_INODE_TMP)) {
        //lc_printf("Converted inode %ld to use a hashed page list, size %ld\n", inode->i_ino, inode->i_size);
        dhpage = lc_malloc(fs, LC_DHASH_MIN * sizeof(struct dhpage *),
                           LC_MEMTYPE_DPAGEHASH);
        memset(dhpage, 0, LC_DHASH_MIN * sizeof(struct dhpage *));
        if (inode->i_pcount) {
            lpage = (inode->i_size + LC_BLOCK_SIZE - 1) /
                    LC_BLOCK_SIZE;
            if (lpage >= inode->i_pcount) {
                lpage = inode->i_pcount - 1;
            }
            for (i = 0; i <= lpage; i++) {
                page = lc_findDirtyPage(inode, i);
                if (page->dp_data == NULL) {
                    continue;
                }
                lc_addDirtyPage(fs, dhpage, i, page->dp_data,
                                page->dp_poffset, page->dp_psize);
            }
            lc_free(fs, inode->i_page, inode->i_pcount * sizeof(struct dpage),
                    LC_MEMTYPE_DPAGEHASH);
            inode->i_page = NULL;
            inode->i_pcount = 0;
        }
        inode->i_hpage = dhpage;
        inode->i_flags |= LC_INODE_HASHED;
        return;
    }
    lpage = (inode->i_size + LC_BLOCK_SIZE - 1) / LC_BLOCK_SIZE;
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
        page = lc_malloc(fs, tsize, LC_MEMTYPE_DPAGEHASH);
        if (inode->i_pcount) {
            size = inode->i_pcount * sizeof(struct dpage);
            memcpy(page, inode->i_page, size);
            memset(&page[inode->i_pcount], 0, tsize - size);
            lc_free(fs, inode->i_page, size, LC_MEMTYPE_DPAGEHASH);
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
lc_getDirtyPage(struct gfs *gfs, struct inode *inode, uint64_t pg) {
    struct dpage *dpage;
    char *pdata;

    dpage = lc_findDirtyPage(inode, pg);
    pdata = dpage ? dpage->dp_data : NULL;
    if (pdata &&
        ((dpage->dp_poffset != 0) || (dpage->dp_psize != LC_BLOCK_SIZE))) {
        lc_fillPage(gfs, inode, dpage, pg);
    }
    return pdata;
}

/* Add or update existing page of the inode with new data provided */
static uint64_t
lc_mergePage(struct gfs *gfs, struct inode *inode, uint64_t pg, char *data,
             uint16_t poffset, uint16_t psize) {
    struct fs *fs = inode->i_fs;
    uint16_t doff, dsize;
    struct dpage *dpage;
    bool fill;

    assert(poffset >= 0);
    assert(poffset < LC_BLOCK_SIZE);
    assert(psize > 0);
    assert(psize <= LC_BLOCK_SIZE);
    assert(!(inode->i_flags & LC_INODE_SHARED));
    assert((inode->i_flags & LC_INODE_HASHED) || (pg < inode->i_pcount));

    dpage = lc_findDirtyPage(inode, pg);
    if (dpage == NULL) {
        assert(inode->i_flags & LC_INODE_HASHED);
        assert(inode->i_pcount == 0);
        lc_addDirtyPage(fs, inode->i_hpage, pg, data, poffset, psize);
        inode->i_dpcount++;
        return 1;
    }

    /* If no dirty page exists, add the new page and return */
    if (dpage->dp_data == NULL) {
        assert(!(inode->i_flags & LC_INODE_HASHED));
        dpage->dp_data = data;
        dpage->dp_poffset = poffset;
        dpage->dp_psize = psize;
        inode->i_dpcount++;
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
            lc_fillPage(gfs, inode, dpage, pg);
        } else {
            dpage->dp_poffset = doff;
            dpage->dp_psize += dsize;
        }
    }
    memcpy(&dpage->dp_data[poffset], &data[poffset], psize);
    lc_free(fs, data, LC_BLOCK_SIZE, LC_MEMTYPE_DATA);
    return 0;
}

/* Copy in provided pages */
uint64_t
lc_copyPages(struct fs *fs, off_t off, size_t size, struct dpage *dpages,
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
        lc_mallocBlockAligned(fs, (void **)&pdata, LC_MEMTYPE_DATA);
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
    struct fs *fs = inode->i_fs;
    struct gfs *gfs = fs->fs_gfs;
    uint64_t page, spage, count = 0;
    off_t endoffset = off + size;
    struct dpage *dpage;
    uint64_t added = 0;

    assert(S_ISREG(inode->i_mode));

    spage = off / LC_BLOCK_SIZE;
    page = spage;

    /* Update inode size if needed */
    if (endoffset > inode->i_size) {
        inode->i_size = endoffset;
    }

    /* Copy page headers if page chain is shared */
    if (inode->i_flags & LC_INODE_SHARED) {
        lc_copyEmap(gfs, fs, inode);
    }
    lc_inodeAllocPages(inode);

    /* Link the dirty pages to the inode, merging with any existing ones */
    while (count < pcount) {
        dpage = &dpages[count];
        added += lc_mergePage(gfs, inode, page, dpage->dp_data,
                              dpage->dp_poffset, dpage->dp_psize);

        /* Flush dirty pages if the inode accumulated too many */
        if ((inode->i_dpcount >= LC_MAX_FILE_DIRTYPAGES) &&
            !(inode->i_flags & LC_INODE_TMP)) {
            lc_flushInodeDirtyPages(inode, page);
        }
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
    struct gfs *gfs = fs->fs_gfs;
    struct page *page = NULL;
    char *data;

    /* XXX Issue a single read if pages are not present in cache */
    assert(S_ISREG(inode->i_mode));
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
        data = lc_getDirtyPage(gfs, inode, pg);
        if (data == NULL) {

            /* Check emap to find the block of the page */
            block = lc_inodeEmapLookup(gfs, inode, pg);
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

/* Flush dirty pages of an inode */
void
lc_flushPages(struct gfs *gfs, struct fs *fs, struct inode *inode,
              bool release) {
    uint64_t count = 0, bcount = 0, start, end = 0, fcount = 0;
    uint64_t i, lpage, pcount, block, tcount = 0, rcount;
    struct page *page, *dpage = NULL, *tpage = NULL;
    struct extent *extents = NULL, *extent;
    bool single = true;
    struct dpage *dp;
    char *pdata;

    assert(S_ISREG(inode->i_mode));
    assert(!(inode->i_flags & LC_INODE_SHARED));

    /* If inode does not have any pages, return */
    if ((inode->i_dpcount == 0) || (inode->i_size == 0)) {
        goto out;
    }
    //assert(!(inode->i_flags & LC_INODE_HASHED));
    lpage = (inode->i_size + LC_BLOCK_SIZE - 1) / LC_BLOCK_SIZE;
    assert((inode->i_flags & LC_INODE_HASHED) || (lpage < inode->i_pcount));

    /* Count the dirty pages and check if whole file can be placed
     * contiguous on disk
     */
    start = lpage + 1;
    for (i = 0; i <= lpage; i++) {
        dp = lc_findDirtyPage(inode, i);
        if (dp && dp->dp_data) {
            start = i;
            break;
        }
    }
    assert(start <= lpage);
    end = lpage + 1;
    for (i = lpage; i >= 0; i--) {
        dp = lc_findDirtyPage(inode, i);
        if (dp && dp->dp_data) {
            end = i;
            break;
        }
    }
    assert(end <= lpage);
    assert(start <= end);
    if (start || ((end + 1) != inode->i_dpcount)) {
        lc_printf("Fragmented file, start %ld end %ld pcount %d\n", start, end, inode->i_dpcount);
        single = false;
    } else if (inode->i_extentLength || inode->i_emap) {
        for (i = start; i <= end; i++) {
            if (lc_inodeEmapLookup(gfs, inode, i) != LC_PAGE_HOLE) {
                single = false;
                break;
            }
        }
    }
    bcount = inode->i_dpcount;
    rcount = bcount;
    do {
        block = lc_blockAlloc(fs, rcount, false, true);
        if (block != LC_INVALID_BLOCK) {
            break;
        }
        rcount /= 2;
    } while ((block == LC_INVALID_BLOCK) && rcount);
    assert(block != LC_INVALID_BLOCK);
    if (bcount != rcount) {
        lc_printf("File system fragmented. Inode %ld is fragmented\n", inode->i_ino);
        single = false;
    }

    /* Check if file has a single extent */
    if (single) {
        assert(inode->i_page[0].dp_data);

        /* Free any old blocks present */
        if (inode->i_extentLength) {
            lc_freeLayerDataBlocks(fs, inode->i_extentBlock,
                                   inode->i_extentLength, inode->i_private);
        } else if (inode->i_emap) {
            extent = inode->i_emap;
            while (extent) {
                assert(extent->ex_type == LC_EXTENT_EMAP);
                lc_validateExtent(gfs, extent);
                lc_addSpaceExtent(gfs, fs, &extents, lc_getExtentBlock(extent),
                                  lc_getExtentCount(extent));
                extent = extent->ex_next;
            }
            inode->i_emap = NULL;
        }
        inode->i_extentBlock = block;
        inode->i_extentLength = bcount;
        inode->i_dinode.di_blocks = bcount;
    } else if (inode->i_extentLength && (start == inode->i_extentLength) &&
               (bcount == rcount) && ((start + bcount - 1) == end)) {

        /* If previous extent is extended, keep the single extent layout */
        single = true;
        assert((inode->i_extentBlock + inode->i_extentLength) == block);
        inode->i_extentLength += bcount;
        inode->i_dinode.di_blocks += bcount;
    } else if (inode->i_extentLength) {
        lc_expandEmap(gfs, fs, inode);
    }

    /* Queue the dirty pages for flushing after associating with newly
     * allocated blocks
     */
    for (i = start; i <= end; i++) {
        if ((count == rcount) && (bcount > tcount)) {
            assert(!single);
            lc_addPageForWriteBack(gfs, fs, dpage, tpage, fcount);
            dpage = NULL;
            tpage = NULL;
            fcount = 0;
            rcount = bcount - tcount;
            do {
                block = lc_blockAlloc(fs, rcount, false, true);
                if (block != LC_INVALID_BLOCK) {
                    break;
                }
                rcount /= 2;
            } while (rcount);
            assert(block != LC_INVALID_BLOCK);
            count = 0;
        }
        pdata = lc_removeDirtyPage(gfs, inode, i, false);
        if (pdata) {
            assert(count < rcount);
            if (!single) {

                /* XXX Do this in one step for contiguous pages */
                lc_inodeEmapUpdate(gfs, fs, inode, i, block + count, &extents);
            }
            page = lc_getPageNew(gfs, fs, block + count, pdata);
            if (tpage == NULL) {
                tpage = page;
            }
            assert(page->p_dnext == NULL);
            page->p_dnext = dpage;
            dpage = page;
            count++;
            fcount++;
            tcount++;

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
    assert(bcount == tcount);
    assert(inode->i_dpcount == 0);

out:

    /* Free dirty page list as all pages are in block cache */
    if (release) {
        if (inode->i_flags & LC_INODE_HASHED) {
            lc_free(fs, inode->i_hpage, LC_DHASH_MIN * sizeof(struct dhpage *),
                    LC_MEMTYPE_DPAGEHASH);
            inode->i_hpage = NULL;
            inode->i_flags &= ~LC_INODE_HASHED;
        } else {
            lc_free(fs, inode->i_page, inode->i_pcount * sizeof(struct dpage),
                    LC_MEMTYPE_DPAGEHASH);
            inode->i_page = NULL;
            inode->i_pcount = 0;
        }
    }
    if (extents) {
        lc_freeInodeDataBlocks(fs, inode, &extents);
    }
    if (tcount) {
        lc_memTransferCount(fs, tcount);
        pcount = __sync_fetch_and_sub(&fs->fs_pcount, tcount);
        assert(pcount >= tcount);
    }
}

/* Truncate a dirty page */
void
lc_truncatePage(struct fs *fs, struct inode *inode, struct dpage *dpage,
                uint64_t pg, uint16_t poffset) {

    if (dpage == NULL) {
        lc_inodeAllocPages(inode);
        dpage = lc_findDirtyPage(inode, pg);
    }

    /* Create a dirty page if one does not exist */
    if (dpage->dp_data == NULL) {
        lc_mallocBlockAligned(fs, (void **)&dpage->dp_data, LC_MEMTYPE_DATA);
        __sync_add_and_fetch(&fs->fs_pcount, 1);
        dpage->dp_poffset = 0;
        dpage->dp_psize = 0;
    } else if ((dpage->dp_poffset + dpage->dp_psize) > poffset) {

        /* Truncate down the valid portion of the page */
        if (dpage->dp_poffset >= poffset) {
            dpage->dp_poffset = 0;
            dpage->dp_psize = 0;
        } else {
            dpage->dp_psize = poffset - dpage->dp_poffset;
        }
    }
}

/* Truncate pages beyond the new size of the file */
void
lc_truncPages(struct inode *inode, off_t size, bool remove) {
    uint64_t pg = size / LC_BLOCK_SIZE, lpage, freed, i, pcount;
    struct dhpage *prev, *hpage;
    bool truncated = false;
    struct dpage *dpage;
    struct gfs *gfs;
    struct fs *fs;

    /* If nothing to truncate, return */
    if ((inode->i_emap == NULL) && (inode->i_dpcount == 0) &&
        (inode->i_extentLength == 0)) {
        assert(inode->i_page == NULL);
        assert(!(inode->i_flags & LC_INODE_SHARED));
        if (remove) {
            assert(inode->i_dinode.di_blocks == 0);
            assert(inode->i_size == 0);
            inode->i_private = true;
        }
        return;
    }
    fs = inode->i_fs;
    gfs = fs->fs_gfs;

    /* Copy bmap list before changing it */
    if (inode->i_flags & LC_INODE_SHARED) {
        assert(inode->i_dpcount == 0);
        if (size == 0) {
            if (remove) {
                inode->i_dinode.di_blocks = 0;
                inode->i_extentBlock = 0;
                inode->i_extentLength = 0;
                inode->i_flags &= ~LC_INODE_SHARED;
                inode->i_private = true;
            }
            inode->i_page = NULL;
            inode->i_pcount = 0;
            inode->i_emap = NULL;
            return;
        }
        lc_copyEmap(gfs, fs, inode);
    }
    assert(!(inode->i_flags & LC_INODE_SHARED));

    /* Free blocks allocated beyond new eof */
    lc_emapTruncate(gfs, fs, inode, size, pg, remove, &truncated);

    /* Remove dirty pages past the new size from the dirty list */
    if (inode->i_dpcount == 0) {
        return;
    }
    freed = 0;
    if (inode->i_flags & LC_INODE_HASHED) {
        for (i = 0; i < LC_DHASH_MIN; i++) {
            prev = NULL;
            hpage = inode->i_hpage[i];
            while (hpage) {
                if ((pg == hpage->dh_pg) && ((size % LC_BLOCK_SIZE) != 0)) {
                    if (!truncated) {
                        lc_truncatePage(fs, inode, &hpage->dh_page, pg,
                                        size % LC_BLOCK_SIZE);
                    }
                } else if (pg <= hpage->dh_pg) {
                    if (prev == NULL) {
                        inode->i_hpage[i] = hpage->dh_next;
                    } else {
                        prev->dh_next = hpage->dh_next;
                    }
                    lc_free(fs, hpage->dh_page.dp_data, LC_BLOCK_SIZE,
                            LC_MEMTYPE_DATA);
                    lc_free(fs, hpage, sizeof(struct dhpage),
                            LC_MEMTYPE_HPAGE);
                    assert(inode->i_dpcount > 0);
                    inode->i_dpcount--;
                    freed++;
                    hpage = prev ? prev->dh_next : inode->i_hpage[i];
                    continue;
                }
                prev = hpage;
                hpage = hpage->dh_next;
            }
        }
        if (size == 0) {
            lc_free(fs, inode->i_hpage, LC_DHASH_MIN * sizeof(struct dhpage *),
                    LC_MEMTYPE_DPAGEHASH);
            inode->i_hpage = NULL;
            inode->i_flags &= ~LC_INODE_HASHED;
        }
    } else if (inode->i_pcount) {
        freed = 0;
        lpage = (inode->i_size + LC_BLOCK_SIZE - 1) / LC_BLOCK_SIZE;
        assert(lpage < inode->i_pcount);
        for (i = pg; i <= lpage; i++) {
            dpage = lc_findDirtyPage(inode, i);
            if (dpage->dp_data == NULL) {
                continue;
            }
            if ((pg == i) && ((size % LC_BLOCK_SIZE) != 0)) {

                /* If a page is partially truncated, keep it */
                if (!truncated) {
                    lc_truncatePage(fs, inode, dpage, pg,
                                    size % LC_BLOCK_SIZE);
                }
            } else {
                lc_removeDirtyPage(gfs, inode, i, true);
                freed++;
            }
        }
        if (size == 0) {
            if (inode->i_page) {
                lc_free(fs, inode->i_page,
                        inode->i_pcount * sizeof(struct dpage),
                        LC_MEMTYPE_DPAGEHASH);
                inode->i_page = NULL;
                inode->i_pcount = 0;
            }
            assert(inode->i_pcount == 0);
        }
    }
    assert((inode->i_dpcount == 0) || size);
    if (freed) {
        pcount = __sync_fetch_and_sub(&fs->fs_pcount, freed);
        assert(pcount >= freed);
    }
}
