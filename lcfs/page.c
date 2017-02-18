#include "includes.h"

/* Calculate the hash index of dirty page */
static inline int
lc_dpageHash(uint64_t page) {
    return page % LC_DHASH_MIN;
}

/* Initialize page markers tracked with the inode to locate first and last
 * dirty page of the inode.
 */
static inline void
lc_initInodePageMarkers(struct inode *inode) {
    lc_inodeSetFirstPage(inode, ((inode->i_size + LC_BLOCK_SIZE - 1) /
                                 LC_BLOCK_SIZE) + 1);
    lc_inodeSetLastPage(inode, 0);
}

/* Update page markers tracked with the inode */
static inline void
lc_updateInodePageMarkers(struct inode *inode, uint64_t pg) {
    if (lc_inodeGetFirstPage(inode) > pg) {
        lc_inodeSetFirstPage(inode, pg);
    }
    if (lc_inodeGetLastPage(inode) < pg) {
        lc_inodeSetLastPage(inode, pg);
    }
}

/* Free data allocated for a page unless it is the zero page */
void
lc_freePageData(struct gfs *gfs, struct fs *fs, char *data) {
    if (data != gfs->gfs_zPage) {
        lc_free(fs, data, LC_BLOCK_SIZE, LC_MEMTYPE_DATA);
    }
}

/* Free pages in the list */
void
lc_freePages(struct fs *fs, struct dpage *dpages, uint64_t pcount) {
    struct gfs *gfs = fs->fs_gfs;
    uint64_t count = 0;

    while (count < pcount) {
        if (dpages[count].dp_data) {
            lc_freePageData(gfs, fs, dpages[count].dp_data);
        }
        count++;
    }
}

/* Add a page to dirty page hash */
static void
lc_addDirtyPage(struct fs *fs, struct dhpage **dhpage, uint64_t page,
                char *data, uint16_t poffset, uint16_t psize) {
    int hash = lc_dpageHash(page);
    struct dhpage *hpage, *next = dhpage[hash], **prev = &dhpage[hash];

    hpage = lc_malloc(fs, sizeof(struct dhpage), LC_MEMTYPE_HPAGE);
    hpage->dh_pg = page;
    hpage->dh_page.dp_data = data;
    hpage->dh_page.dp_poffset = poffset;
    hpage->dh_page.dp_psize = psize;
    hpage->dh_page.dp_pread = 0;

    /* Keep the list sorted */
    while (next && (page < next->dh_pg)) {
        prev = &next->dh_next;
        next = next->dh_next;
    }
    assert((next == NULL) || (next->dh_pg != page));
    hpage->dh_next = next;
    *prev = hpage;
}

/* Return the requested page if allocated already */
static inline struct dpage *
lc_findDirtyPage(struct inode *inode, uint64_t pg) {
    struct dhpage *dhpage;
    int hash;

    if (inode->i_flags & LC_INODE_HASHED) {
        assert(lc_inodeGetPageCount(inode) == 0);

        /* If the page is not between first and last pages, return */
        if ((lc_inodeGetDirtyPageCount(inode) == 0) || (pg < lc_inodeGetFirstPage(inode)) ||
            (pg > lc_inodeGetLastPage(inode))) {
            return NULL;
        }
        hash = lc_dpageHash(pg);
        dhpage = inode->i_hpage[hash];
        while (dhpage) {
            if (dhpage->dh_pg == pg) {
                return &dhpage->dh_page;
            }

            /* List is sorted */
            if (pg > dhpage->dh_pg) {
                break;
            }
            dhpage = dhpage->dh_next;
        }
        return NULL;
    }
    return (pg < lc_inodeGetPageCount(inode)) ? &inode->i_page[pg] : NULL;
}

/* Flush dirty pages if the inode accumulated too many */
bool
lc_flushInodeDirtyPages(struct inode *inode, uint64_t page, bool unlock,
                        bool force) {
    struct dpage *dpage;

    /* Do not trigger flush if the last page is not fully filled up for a
     * sequentially written file.
     */
    if (!force && (inode->i_extentLength || (lc_inodeGetEmap(inode) == NULL))) {
        dpage = lc_findDirtyPage(inode, page);
        if ((dpage == NULL) ||
            (dpage->dp_data &&
             (dpage->dp_poffset || (dpage->dp_psize != LC_BLOCK_SIZE)))) {
            return false;
        }
    }
    lc_flushPages(inode->i_fs->fs_gfs, inode->i_fs, inode, false, false, unlock);
    return true;
}

/* Add inode to the layer dirty list if it is not already in it */
void
lc_addDirtyInode(struct fs *fs, struct inode *inode) {
    assert(S_ISREG(inode->i_mode));
    pthread_mutex_lock(&fs->fs_dilock);
    if ((lc_inodeGetDirtyNext(inode) == NULL) && (fs->fs_dirtyInodesLast != inode)) {
        if (fs->fs_dirtyInodesLast) {
            lc_inodeSetDirtyNext(fs->fs_dirtyInodesLast, inode);
        } else {
            assert(fs->fs_dirtyInodes == NULL);
            fs->fs_dirtyInodes = inode;
        }
        fs->fs_dirtyInodesLast = inode;
    }
    pthread_mutex_unlock(&fs->fs_dilock);
}

/* Remove an inode from the layer dirty list */
static struct inode *
lc_removeDirtyInode(struct fs *fs, struct inode *inode, struct inode *prev) {
    struct inode *next = lc_inodeGetDirtyNext(inode);

    assert(S_ISREG(inode->i_mode));
    if (prev) {
        lc_inodeSetDirtyNext(prev, next);
    } else {
        assert(fs->fs_dirtyInodes == inode);
        fs->fs_dirtyInodes = next;
    }
    if (fs->fs_dirtyInodesLast == inode) {
        assert(prev || (fs->fs_dirtyInodes == NULL));
        fs->fs_dirtyInodesLast = prev;
    }
    lc_inodeSetDirtyNext(inode, NULL);
    return next;
}

/* Flush inodes on the dirty list */
void
lc_flushDirtyInodeList(struct fs *fs, bool all) {
    struct inode *inode, *prev = NULL, *next;
    bool flushed, force;
    uint64_t id;

    if ((fs->fs_dirtyInodes == NULL) || fs->fs_removed) {
        return;
    }
    force = all || (fs->fs_pcount > LC_MAX_LAYER_DIRTYPAGES);
    pthread_mutex_lock(&fs->fs_dilock);

    /* Increment flusher id and store it with inodes flushed by this thread.
     * This helps to avoid processing same inodes over and over.
     */
    id = ++fs->fs_flusher;
    inode = fs->fs_dirtyInodes;
    while (inode && !fs->fs_removed) {
        assert(inode->i_fs == fs);

        /* Stop if remaining inodes are recently processed and not ready yet */
        if (lc_inodeGetFlusher(inode) >= id) {
            break;
        }
        lc_inodeSetFlusher(inode, id);
        if (inode->i_flags & LC_INODE_REMOVED) {

            /* Take removed inodes off of the dirty list */
            inode = lc_removeDirtyInode(fs, inode, prev);
        } else if (!pthread_rwlock_trywrlock(inode->i_rwlock)) {
            next = lc_removeDirtyInode(fs, inode, prev);
            if (lc_inodeGetDirtyPageCount(inode) && !(inode->i_flags & LC_INODE_REMOVED) &&
                ((inode->i_ocount == 0) || force)) {
                pthread_mutex_unlock(&fs->fs_dilock);

                /* Attempt to flush dirty inodes if lock is available and no
                 * thread has the file open.
                 */
                flushed = lc_flushInodeDirtyPages(inode,
                                         inode->i_size / LC_BLOCK_SIZE, true,
                                         force);
                if (!flushed) {

                    /* If inode could not be flushed, add it back to the list
                     */
                    if (lc_inodeGetDirtyPageCount(inode) && (inode->i_ocount == 0)) {
                        lc_addDirtyInode(fs, inode);
                    }
                    pthread_rwlock_unlock(inode->i_rwlock);
                } else if (!all &&
                           (fs->fs_pcount < (LC_MAX_LAYER_DIRTYPAGES / 2))) {
                    return;
                }
            } else {
                pthread_rwlock_unlock(inode->i_rwlock);
                inode = next;
                continue;
            }
            pthread_mutex_lock(&fs->fs_dilock);
            prev = NULL;
            inode = fs->fs_dirtyInodes;
        } else {
            prev = inode;
            inode = lc_inodeGetDirtyNext(inode);
        }
    }
    pthread_mutex_unlock(&fs->fs_dilock);
}

/* Fill up a partial page */
static void
lc_fillPage(struct gfs *gfs, struct inode *inode, struct dpage *dpage,
            uint64_t pg, struct extent **extents) {
    uint16_t poffset, psize, dsize, eof;
    struct page *bpage = NULL;
    char *data, *pdata;
    uint64_t block;

    poffset = dpage->dp_poffset;
    psize = dpage->dp_psize;
    pdata = dpage->dp_data;
    assert(pdata != gfs->gfs_zPage);

    /* If the page is written partially, check if a block exists for the page.
     * If there is one, read that block in.
     */
    if ((poffset != 0) ||
        (((pg * LC_BLOCK_SIZE) + psize) < inode->i_size)) {
        block = lc_inodeEmapLookup(gfs, inode, pg, extents);
        if (block != LC_PAGE_HOLE) {
            bpage = lc_getPage(inode->i_fs, block, NULL, true);
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
        dpage->dp_psize += poffset;
    }
    dsize = poffset + psize;
    if (dsize != LC_BLOCK_SIZE) {
        dsize = LC_BLOCK_SIZE - dsize;
        dpage->dp_psize += dsize;
        if (data) {
            eof = (pg == (inode->i_size / LC_BLOCK_SIZE)) ?
                  (inode->i_size % LC_BLOCK_SIZE) : 0;
            if (eof) {

                /* Zero out the page past the eof of the file */
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
    }
    assert(dpage->dp_psize == LC_BLOCK_SIZE);
    if (bpage) {
        lc_releasePage(inode->i_fs->fs_gfs, inode->i_fs, bpage, true);
    }
}

/* Remove a dirty page from the inode's list */
static inline char *
lc_removeDirtyPage(struct gfs *gfs, struct inode *inode, uint64_t pg,
                   bool release, bool *read) {
    struct dhpage *dhpage = NULL, **prev;
    struct fs *fs = inode->i_fs;
    struct dpage *page;
    char *pdata;
    int hash;

    assert(pg >= lc_inodeGetFirstPage(inode));
    assert(pg <= lc_inodeGetLastPage(inode));
    assert(lc_inodeGetDirtyPageCount(inode));
    if (inode->i_flags & LC_INODE_HASHED) {
        assert(lc_inodeGetPageCount(inode) == 0);
        page = NULL;
        hash = lc_dpageHash(pg);
        prev = &inode->i_hpage[hash];
        dhpage = inode->i_hpage[hash];
        while (dhpage) {
            if (dhpage->dh_pg == pg) {
                *prev = dhpage->dh_next;
                page = &dhpage->dh_page;
                break;
            }
            if (pg > dhpage->dh_pg) {
                break;
            }
            prev = &dhpage->dh_next;
            dhpage = dhpage->dh_next;
        }
    } else {
        assert(pg < lc_inodeGetPageCount(inode));
        page = &inode->i_page[pg];
    }
    if (page) {
        pdata = page->dp_data;
        if (read) {
            *read = page->dp_pread;
        }
    } else {
        pdata = NULL;
    }
    if (pdata) {
        if (release) {

            /* Release the page */
            lc_freePageData(gfs, fs, pdata);
        } else if ((page->dp_poffset != 0) ||
                   (page->dp_psize != LC_BLOCK_SIZE)) {

            /* Fill up a partial page before returning */
            lc_fillPage(gfs, inode, page, pg, NULL);
        }
        page->dp_data = NULL;
        lc_inodeDecrDirtyPageCount(inode);
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

    if (inode->i_flags & LC_INODE_HASHED) {
        assert(lc_inodeGetPageCount(inode) == 0);
        return;
    }

    /* Adapt to a hashing scheme when file grows bigger than a certain size.
     * This is not done for temporary files.
     */
    if ((inode->i_size > (LC_DHASH_MIN * LC_BLOCK_SIZE)) &&
        !(inode->i_flags & LC_INODE_TMP)) {
        //lc_printf("Converted inode %ld to use a hashed page list, size %ld\n", inode->i_ino, inode->i_size);

        /* XXX Adjust hash table size based on file size */
        dhpage = lc_malloc(fs, LC_DHASH_MIN * sizeof(struct dhpage *),
                           LC_MEMTYPE_DPAGEHASH);
        memset(dhpage, 0, LC_DHASH_MIN * sizeof(struct dhpage *));

        /* Move all dirty pages to the hash table */
        if (lc_inodeGetPageCount(inode)) {
            if (lc_inodeGetDirtyPageCount(inode)) {
                assert(lc_inodeGetFirstPage(inode) < lc_inodeGetPageCount(inode));
                assert(lc_inodeGetLastPage(inode) < lc_inodeGetPageCount(inode));
                for (i = lc_inodeGetFirstPage(inode);
                     i <= lc_inodeGetLastPage(inode) ; i++) {
                    page = lc_findDirtyPage(inode, i);
                    if (page->dp_data == NULL) {
                        continue;
                    }
                    lc_addDirtyPage(fs, dhpage, i, page->dp_data,
                                    page->dp_poffset, page->dp_psize);
                }
            }
            lc_free(fs, inode->i_page, lc_inodeGetPageCount(inode) * sizeof(struct dpage),
                    LC_MEMTYPE_DPAGEHASH);
            inode->i_page = NULL;
            lc_inodeSetPageCount(inode, 0);
        }
        inode->i_hpage = dhpage;
        inode->i_flags |= LC_INODE_HASHED;
        return;
    }
    lpage = (inode->i_size + LC_BLOCK_SIZE - 1) / LC_BLOCK_SIZE;
    if (lc_inodeGetPageCount(inode) <= lpage) {

        /* Double the size of the list everytime inode is grown beyond the size
         * of the list.
         */
        count = lc_inodeGetPageCount(inode) ? (lc_inodeGetPageCount(inode) * 2) :
                                  (lpage ? (lpage + 1) : LC_PAGECACHE_SIZE);
        while (count <= lpage) {
            count *= 2;
        }
        tsize = count * sizeof(struct dpage);
        page = lc_malloc(fs, tsize, LC_MEMTYPE_DPAGEHASH);

        /* Copy current page table and zero out rest of the entries */
        if (lc_inodeGetPageCount(inode)) {
            size = lc_inodeGetPageCount(inode) * sizeof(struct dpage);
            memcpy(page, inode->i_page, size);
            memset(&page[lc_inodeGetPageCount(inode)], 0, tsize - size);
            lc_free(fs, inode->i_page, size, LC_MEMTYPE_DPAGEHASH);
        } else {
            assert(inode->i_page == NULL);
            memset(page, 0, tsize);
        }
        lc_inodeSetPageCount(inode, count);
        inode->i_page = page;
    }
    assert(lpage <= lc_inodeGetPageCount(inode));
}

/* Get current page dirty list filled up with valid data */
static char *
lc_getDirtyPage(struct gfs *gfs, struct inode *inode, uint64_t pg,
                struct extent **extents) {
    struct dpage *dpage;
    char *pdata;

    dpage = lc_findDirtyPage(inode, pg);
    pdata = dpage ? dpage->dp_data : NULL;
    if (pdata) {
        if ((dpage->dp_poffset != 0) || (dpage->dp_psize != LC_BLOCK_SIZE)) {

            /* Fill up a partial page */
            lc_fillPage(gfs, inode, dpage, pg, extents);
        }
        dpage->dp_pread = 1;
    }
    return pdata;
}

/* Add or update existing page of the inode with new data provided */
static int
lc_mergePage(struct gfs *gfs, struct inode *inode, uint64_t pg,
             struct dpage *page, struct extent **extents) {
    uint64_t poffset = page->dp_poffset, psize = page->dp_psize;
    char *data = page->dp_data;
    struct fs *fs = inode->i_fs;
    uint16_t doff, dsize;
    struct dpage *dpage;
    bool fill;

    assert(poffset >= 0);
    assert(poffset < LC_BLOCK_SIZE);
    assert(psize > 0);
    assert(psize <= LC_BLOCK_SIZE);
    assert((inode->i_flags & LC_INODE_HASHED) || (pg < lc_inodeGetPageCount(inode)));

    /* Check if the block is full of zeros */
    if ((poffset == 0) && (psize == LC_BLOCK_SIZE) &&
        !memcmp(data, gfs->gfs_zPage, LC_BLOCK_SIZE)) {
        data = gfs->gfs_zPage;
    }
    dpage = lc_findDirtyPage(inode, pg);
    if (dpage == NULL) {

        /* If no dirty page exists, add this one and return */
        assert(inode->i_flags & LC_INODE_HASHED);
        assert(lc_inodeGetPageCount(inode) == 0);
        lc_addDirtyPage(fs, inode->i_hpage, pg, data, poffset, psize);
        lc_inodeIncrDirtyPageCount(inode);
        lc_updateInodePageMarkers(inode, pg);
        if (data != gfs->gfs_zPage) {
            page->dp_data = NULL;
        }
        return 1;
    }
    dpage->dp_pread = 0;

    /* If no dirty page exists, add the new page and return */
    if (dpage->dp_data == NULL) {
        assert(!(inode->i_flags & LC_INODE_HASHED));
        dpage->dp_data = data;
        dpage->dp_poffset = poffset;
        dpage->dp_psize = psize;
        lc_inodeIncrDirtyPageCount(inode);
        lc_updateInodePageMarkers(inode, pg);
        if (data != gfs->gfs_zPage) {
            page->dp_data = NULL;
        }
        return 1;
    }
    assert(lc_inodeGetFirstPage(inode) <= pg);
    assert(lc_inodeGetLastPage(inode) >= pg);

    /* Check if replacing a zero page */
    if (dpage->dp_data == gfs->gfs_zPage) {
        assert(dpage->dp_poffset == 0);
        assert(dpage->dp_psize == LC_BLOCK_SIZE);
        if (data != gfs->gfs_zPage) {
            dpage->dp_data = data;
            if (poffset) {
                memset(data, 0, poffset);
            }
            dsize = poffset + psize;
            if (dsize != LC_BLOCK_SIZE) {
                memset(&data[dsize], 0, LC_BLOCK_SIZE - dsize);
            }
            page->dp_data = NULL;
        }
        return 0;
    }

    /* Check if replacing with a zero page */
    if (data == gfs->gfs_zPage) {
        dpage->dp_data = data;
        dpage->dp_poffset = 0;
        dpage->dp_psize = LC_BLOCK_SIZE;
        return 0;
    }

    /* If the current dirty page is partial page and this new write is not a
     * contiguous write, initialize exisitng page correctly before copying new
     * data.
     */
    if (((dpage->dp_poffset != 0) || (dpage->dp_psize != LC_BLOCK_SIZE)) &&
        ((poffset < dpage->dp_poffset) ||
         ((poffset + psize) > (dpage->dp_poffset + dpage->dp_psize)))) {
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

            /* Fill the page if this write is not adjacent to current valid
             * portion of the page.
             */
            lc_fillPage(gfs, inode, dpage, pg, extents);
        } else {
            dpage->dp_poffset = doff;
            dpage->dp_psize += dsize;
        }
    }
    assert(dpage->dp_data != gfs->gfs_zPage);
    memcpy(&dpage->dp_data[poffset], &data[poffset], psize);
    return 0;
}

/* Copy in provided data into page aligned buffers */
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

    /* Read data from fuse */
    wsize = fuse_buf_copy(dst, bufv, FUSE_BUF_SPLICE_MOVE);
    assert(wsize == size);
    return pcount;
}

/* Fill up the last page with zeroes when a file grows */
static void
lc_zeroFillLastPage(struct gfs *gfs, struct inode *inode) {
    uint64_t pg = inode->i_size / LC_BLOCK_SIZE, block;
    struct dpage *dpage;
    uint16_t off;

    /* Check if a block exists for the last page */
    if (inode->i_dinode.di_blocks == 0) {
        return;
    }
    block = lc_inodeEmapLookup(gfs, inode, pg, NULL);
    if (block == LC_PAGE_HOLE) {
        return;
    }
    dpage = lc_findDirtyPage(inode, pg);
    if (dpage && dpage->dp_data) {
        off = dpage->dp_poffset + dpage->dp_psize;
        if (off != LC_BLOCK_SIZE) {
            memset(&dpage->dp_data[off], 0, LC_BLOCK_SIZE - off);
            dpage->dp_psize = LC_BLOCK_SIZE - dpage->dp_poffset;
        }
    }
}

/* Update pages of a file with provided data */
uint64_t
lc_addPages(struct inode *inode, off_t off, size_t size,
            struct dpage *dpages, uint64_t pcount) {
    uint64_t page = off / LC_BLOCK_SIZE, count = 0;
    struct extent *extent = lc_inodeGetEmap(inode);
    struct fs *fs = inode->i_fs;
    struct gfs *gfs = fs->fs_gfs;
    off_t endoffset = off + size;
    struct dpage *dpage;
    uint64_t added = 0;

    assert(S_ISREG(inode->i_mode));

    /* Update inode size if needed */
    if (endoffset > inode->i_size) {

        /* Zero fill last page if that is a partial page */
        if ((off > inode->i_size) && (inode->i_size % LC_BLOCK_SIZE)) {
            lc_zeroFillLastPage(gfs, inode);
        }
        inode->i_size = endoffset;
    }

    /* Skip the write if incoming data is fully zeroes and the file empty */
    if ((inode->i_dinode.di_blocks == 0) && (lc_inodeGetDirtyPageCount(inode) == 0)) {
        while (count < pcount) {
            dpage = &dpages[count];
            if (memcmp(&dpage->dp_data[dpage->dp_poffset], gfs->gfs_zPage,
                       dpage->dp_psize)) {
                break;
            }
            count++;
        }
        if (count == pcount) {
            return 0;
        }
        count = 0;
    }

    if (lc_inodeGetDirtyPageCount(inode) == 0) {

        /* Initialize first and last page markers when the first dirty page
         * added.
         */
        lc_initInodePageMarkers(inode);
    }
    lc_inodeAllocPages(inode);

    /* Link the dirty pages to the inode, merging with any existing ones */
    while (count < pcount) {
        dpage = &dpages[count];
        added += lc_mergePage(gfs, inode, page, dpage, &extent);

        /* Flush dirty pages if the inode accumulated too many */
        if ((lc_inodeGetDirtyPageCount(inode) >= LC_MAX_FILE_DIRTYPAGES) &&
            !(inode->i_flags & LC_INODE_TMP)) {
            lc_flushInodeDirtyPages(inode, page, false, false);
        }
        page++;
        count++;
    }
    return added;
}

/* Read specified pages of a file */
int
lc_readFile(fuse_req_t req, struct fs *fs, struct inode *inode, off_t soffset,
            off_t endoffset, uint64_t asize, struct page **pages, char **dbuf,
            struct fuse_bufvec *bufv) {
    uint64_t block, pg = soffset / LC_BLOCK_SIZE, pcount = 0, dcount = 0;
    struct extent *extent = lc_inodeGetEmap(inode);
    size_t psize, rsize = endoffset - soffset;
    struct page *page = NULL, **rpages = NULL;
    off_t poffset, off = soffset;
    struct gfs *gfs = fs->fs_gfs;
    uint32_t count, rcount = 0;
    uint64_t i = 0;
    char *data;

    assert(S_ISREG(inode->i_mode));
    count = (rsize / LC_BLOCK_SIZE) + 2;
    while (rsize) {
        assert(pg == (off / LC_BLOCK_SIZE));
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
        assert((off + psize) <= inode->i_size);

        /* Check if a dirty page exists */
        data = lc_getDirtyPage(gfs, inode, pg, &extent);
        if (data == NULL) {

            /* Check emap to find the block of the page
             * XXX Avoid emap lookup by maintaining a hash table for
             * <inode, page> lookup.
             */
            block = lc_inodeEmapLookup(gfs, inode, pg, &extent);
            if (block == LC_PAGE_HOLE) {
                bufv->buf[i].mem = gfs->gfs_zPage;
            } else {
                page = lc_getPageNewData(fs, block, dbuf ? dbuf[dcount] : NULL);

                /* If page is missing a data buffer, allocate one after
                 * unlocking the inode.
                 */
                if (!page->p_dvalid && (page->p_data == NULL)) {
                    lc_releaseReadPages(gfs, fs, pages, pcount, false);
                    return ENOMEM;
                }
                if (dbuf && (page->p_data == dbuf[dcount])) {
                    dcount++;
                }
                bufv->buf[i].mem = &page->p_data[poffset];
                pages[pcount++] = page;

                /* If page does not have valid data, add the page to the list
                 * for reading from disk.
                 */
                if (!page->p_dvalid) {
                    if (rpages == NULL) {
                        rpages = alloca(count * sizeof(struct page *));
                    }
                    rpages[rcount] = page;
                    rcount++;
                }
            }
        } else {
            bufv->buf[i].mem = &data[poffset];
        }
        bufv->buf[i].size = psize;
        count--;
        i++;
        pg++;
        off += psize;
        rsize -= psize;
    }
    assert(i <= asize);
    assert(pcount <= asize);
    bufv->count = i;

    /* Read in any pages without valid data associated with */
    if (rcount) {
        lc_readPages(gfs, fs, rpages, rcount);
    }
    fuse_reply_data(req, bufv, FUSE_BUF_SPLICE_MOVE);
    lc_inodeUnlock(inode);
    lc_releaseReadPages(gfs, fs, pages, pcount, false);
#if 0
                        ((inode->i_fs == fs) && inode->i_private) ||
                        (fs->fs_parent && fs->fs_parent->fs_single));
#endif
    if (dbuf) {
        while (dcount < pcount) {
            lc_freePageData(gfs, fs->fs_rfs, dbuf[dcount]);
            dcount++;
        }
    }
    return 0;
}

/* Flush dirty pages of an inode */
void
lc_flushPages(struct gfs *gfs, struct fs *fs, struct inode *inode,
              bool io, bool release, bool unlock) {
    uint64_t count = 0, bcount, start, end = 0, pstart = -1, zcount = 0;
    uint64_t lpage, pcount = 0, tcount = 0, rcount = 0, bstart = -1;
    uint64_t eblock = LC_INVALID_BLOCK, elength = 0, dblocks = 0;
    struct page *page, *dpage = NULL, *tpage = NULL;
    uint64_t fcount = 0, block = LC_INVALID_BLOCK;
    struct extent *extents = NULL, *extent, *tmp;
    bool single, read;
    char *pdata;
    int64_t i;

    /* XXX Avoid holding inode lock exclusive in this function */
    assert(S_ISREG(inode->i_mode));

    /* If inode does not have any pages, skip most of the work */
    if ((lc_inodeGetDirtyPageCount(inode) == 0) || (inode->i_size == 0)) {
        goto out;
    }
    assert((inode->i_dinode.di_blocks == 0) ||
           (inode->i_extentLength == inode->i_dinode.di_blocks) ||
           lc_inodeGetEmap(inode));
    lpage = (inode->i_size + LC_BLOCK_SIZE - 1) / LC_BLOCK_SIZE;
    if (!(inode->i_flags & LC_INODE_HASHED) && (lc_inodeGetPageCount(inode) < lpage)) {
        assert(lc_inodeGetPageCount(inode) > lc_inodeGetDirtyPageCount(inode));
        lpage = lc_inodeGetPageCount(inode) - 1;
    }
    bcount = lc_inodeGetDirtyPageCount(inode);
    start = lc_inodeGetFirstPage(inode);
    end = lc_inodeGetLastPage(inode);
    assert(start < lpage);
    assert(end < lpage);
    assert(start <= end);
    assert(bcount <= (end - start + 1));

    /* Allocate blocks.  If a single extent cannot be allocated, allocate
     * smaller chunks.
     */
    rcount = bcount;
    do {
        block = lc_blockAlloc(fs, rcount, false, true);
        if (block != LC_INVALID_BLOCK) {
            break;
        }
        rcount /= 2;
    } while (rcount);
    assert(block != LC_INVALID_BLOCK);
    if (bcount != rcount) {
        single = false;
        lc_printf("File system fragmented. Inode %ld is fragmented\n", inode->i_ino);
    } else {

        /* File being written sequentially from the beginning can be placed
         * contiguous on disk.
         */
        single = (start == 0) && ((end + 1) == bcount) &&
                 ((inode->i_dinode.di_blocks == 0) ||
                  (((end + 1) == lpage) &&
                   (inode->i_dinode.di_blocks <= bcount)));
        if (!single) {
            lc_printf("Fragmented file %ld, size %ld start %ld end %ld lpage %ld blocks %d pages %ld\n",
                      inode->i_ino, inode->i_size, start, end, lpage, inode->i_dinode.di_blocks, bcount);
        }
    }

    /* Make a private copy of the emap list if inode is fragmented */
    if (inode->i_flags & LC_INODE_SHARED) {
        lc_copyEmap(gfs, fs, inode);
    }

    /* Check if file has a single extent */
    if (single) {
        assert(inode->i_page[0].dp_data);
        eblock = block;
        elength = bcount;
        dblocks = bcount;
    } else if (inode->i_extentLength && (start == inode->i_extentLength) &&
               ((inode->i_extentBlock + inode->i_extentLength) == block) &&
               (bcount == rcount) && ((start + bcount - 1) == end)) {

        /* If previous extent is extended, keep the single extent layout */
        single = true;
        eblock = inode->i_extentBlock;
        elength = inode->i_extentLength + bcount;
        dblocks = inode->i_dinode.di_blocks + bcount;
    } else if (inode->i_extentLength) {

        /* Expand from a single direct extent to an extent list */
        lc_expandEmap(gfs, fs, inode);
    }

    /* Queue the dirty pages for flushing after associating with newly
     * allocated blocks
     */
    for (i = start; i <= end; i++) {
        if ((count == rcount) && (bcount > tcount)) {
            assert(!single);

            /* Update emap with last extent */
            lc_inodeEmapUpdate(gfs, fs, inode, pstart, bstart,
                               pcount, &extents);

            /* Allocate more blocks */
            rcount = bcount - tcount;
            do {
                block = lc_blockAlloc(fs, rcount, false, true);
                if (block != LC_INVALID_BLOCK) {
                    break;
                }
                rcount /= 2;
            } while (rcount);
            assert(block != LC_INVALID_BLOCK);
            pcount = 0;
            count = 0;
        }
        assert(count < rcount);

        /* Find the next dirty page */
        pdata = lc_removeDirtyPage(gfs, inode, i, false, &read);
        if (pdata) {
            assert(count < rcount);
            if (!single) {
                if (pdata == gfs->gfs_zPage) {

                    /* Punch a hole if zeroes are written */
                    lc_inodeEmapUpdate(gfs, fs, inode, i, LC_PAGE_HOLE, 1,
                                       &extents);
                    fcount++;
                    continue;
                }
                if (pcount == 0) {
                    pstart = i;
                    bstart = block + count;
                }
                if ((pstart + pcount) != i) {

                    /* Add current extent if next page is not contiguous */
                    lc_inodeEmapUpdate(gfs, fs, inode, pstart, bstart,
                                       pcount, &extents);
                    pstart = i;
                    bstart = block + count;
                    pcount = 1;
                } else {
                    pcount++;
                }
            } else if (pdata == gfs->gfs_zPage) {

                /* Zeros are written for single direct extent to avoid
                 * emap fragmentation.
                 */
                zcount++;
            }
            page = lc_getPageNew(gfs, fs, block + count, pdata);

            if (read) {
                page->p_hitCount++;
            }
            if (tpage == NULL) {
                tpage = page;
            }
            assert(page->p_dnext == NULL);
            page->p_dnext = dpage;
            dpage = page;
            count++;
            tcount++;
        }
    }
    if (pcount) {
        assert(!single);

        /* Insert last extent */
        lc_inodeEmapUpdate(gfs, fs, inode, pstart, bstart, pcount, &extents);
    }
    if (single) {

        /* Free any old blocks present */
        if (inode->i_extentLength) {
            if (inode->i_extentBlock != eblock) {
                lc_freeLayerDataBlocks(fs, inode->i_extentBlock,
                                       inode->i_extentLength,
                                       inode->i_private);
            }
        } else if (lc_inodeGetEmap(inode)) {

            /* Traverse the extent list and free every extent */
            extent = lc_inodeGetEmap(inode);
            while (extent) {
                assert(extent->ex_type == LC_EXTENT_EMAP);
                lc_validateExtent(gfs, extent);
                lc_addSpaceExtent(gfs, fs, &extents, lc_getExtentBlock(extent),
                                  lc_getExtentCount(extent), false);
                tmp = extent;
                extent = extent->ex_next;
                lc_free(fs, tmp, sizeof(struct extent), LC_MEMTYPE_EXTENT);
            }
            lc_inodeSetEmap(inode, NULL);
        }
        inode->i_extentBlock = eblock;
        inode->i_extentLength = elength;
        inode->i_dinode.di_blocks = dblocks;
    }
    assert(bcount == (tcount + fcount));
    assert(lc_inodeGetDirtyPageCount(inode) == 0);

out:

    /* Free dirty page list as all pages are in block cache */
    if (release) {
        if (inode->i_flags & LC_INODE_HASHED) {
            lc_free(fs, inode->i_hpage, LC_DHASH_MIN * sizeof(struct dhpage *),
                    LC_MEMTYPE_DPAGEHASH);
            inode->i_hpage = NULL;
            inode->i_flags &= ~LC_INODE_HASHED;
        } else if (lc_inodeGetPageCount(inode)) {
            lc_free(fs, inode->i_page, lc_inodeGetPageCount(inode) * sizeof(struct dpage),
                    LC_MEMTYPE_DPAGEHASH);
            inode->i_page = NULL;
            lc_inodeSetPageCount(inode, 0);
        }
    }
    if (extents) {

        /* Free blocks which are overwritten with new data and no longer in
         * use.
         */
        lc_freeInodeDataBlocks(fs, inode, &extents);
    }
    lc_initInodePageMarkers(inode);

    /* Unlock the inode before issuing I/Os if we can.  As we never overwrite
     * in place, it is safe to do so.
     */
    if (unlock) {
        lc_inodeUnlock(inode);
    }
    if (tcount > zcount) {

        /* Transfer the ownership of dirty pages from the layer to base layer
         */
        lc_memTransferCount(fs, tcount - zcount);
    }
    if (tcount) {

        /* Queue the dirty pages for write */
        lc_addPageForWriteBack(gfs, fs, dpage, tpage, tcount, io);
    }
    tcount += fcount;
    if (tcount) {
        pcount = __sync_fetch_and_sub(&fs->fs_pcount, tcount);
        assert(pcount >= tcount);
        pcount = __sync_fetch_and_sub(&gfs->gfs_dcount, tcount);
        assert(pcount >= tcount);
    }
    if (rcount > count) {

        /* If some blocks are not used when zero pages are skipped, return
         * those back to the free pool.
         */
        lc_blockFree(gfs, fs, block + count, rcount - count, true);
    }
}

/* Truncate a dirty page */
static void
lc_truncatePage(struct gfs *gfs, struct fs *fs, struct inode *inode,
                uint64_t pg, uint16_t poffset, bool create) {

    struct dpage *dpage;

    lc_inodeAllocPages(inode);
    dpage = lc_findDirtyPage(inode, pg);
    if (dpage == NULL) {
        if (!create) {
            return;
        }
        assert(inode->i_flags & LC_INODE_HASHED);
        lc_addDirtyPage(fs, inode->i_hpage, pg, NULL, 0, 0);
        dpage = lc_findDirtyPage(inode, pg);
    }

    /* Create a dirty page if one does not exist */
    if (dpage->dp_data == NULL) {
        if (!create) {
            return;
        }
        lc_mallocBlockAligned(fs, (void **)&dpage->dp_data, LC_MEMTYPE_DATA);
        lc_inodeIncrDirtyPageCount(inode);
        lc_updateInodePageMarkers(inode, pg);
        __sync_add_and_fetch(&fs->fs_pcount, 1);
        __sync_add_and_fetch(&gfs->gfs_dcount, 1);
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

/* Remove dirty pages past the new size from the dirty list */
static void
lc_invalidatePages(struct gfs *gfs, struct fs *fs, struct inode *inode,
                   off_t size) {
    uint64_t pg = size / LC_BLOCK_SIZE, lpage, freed, pcount;
    struct dhpage **next, *hpage;
    struct dpage *dpage;
    int64_t i;

    if (size && (lc_inodeGetDirtyPageCount(inode) == 0)) {
        return;
    }
    freed = 0;
    if (inode->i_flags & LC_INODE_HASHED) {
        for (i = 0; (i < LC_DHASH_MIN) && lc_inodeGetDirtyPageCount(inode); i++) {
            next = &inode->i_hpage[i];
            hpage = inode->i_hpage[i];
            while (hpage) {
                if ((hpage->dh_pg < pg) ||
                    ((hpage->dh_pg == pg) && (size % LC_BLOCK_SIZE))) {

                    /* Processed all pages after the new last page in this list
                     */
                    break;
                }

                /* Free this page */
                *next = hpage->dh_next;
                lc_freePageData(gfs, fs, hpage->dh_page.dp_data);
                lc_free(fs, hpage, sizeof(struct dhpage), LC_MEMTYPE_HPAGE);
                assert(lc_inodeGetDirtyPageCount(inode) > 0);
                lc_inodeDecrDirtyPageCount(inode);
                freed++;
                hpage = *next;
            }
        }
        if (size == 0) {
            lc_free(fs, inode->i_hpage, LC_DHASH_MIN * sizeof(struct dhpage *),
                    LC_MEMTYPE_DPAGEHASH);
            inode->i_hpage = NULL;
            inode->i_flags &= ~LC_INODE_HASHED;
        }
    } else if (lc_inodeGetPageCount(inode)) {
        lpage = lc_inodeGetLastPage(inode);
        assert(lpage < lc_inodeGetPageCount(inode));

        /* Remove all pages after the new last page */
        for (i = pg; i <= lpage; i++) {
            dpage = lc_findDirtyPage(inode, i);
            if (dpage->dp_data == NULL) {
                continue;
            }

            /* Don't remove last paget if that is partially truncated */
            if ((i > pg) || ((size % LC_BLOCK_SIZE) == 0)) {
                lc_removeDirtyPage(gfs, inode, i, true, NULL);
                freed++;
            }
        }
        if (size == 0) {
            lc_free(fs, inode->i_page, lc_inodeGetPageCount(inode) * sizeof(struct dpage),
                    LC_MEMTYPE_DPAGEHASH);
            inode->i_page = NULL;
            lc_inodeSetPageCount(inode, 0);
        }
    }
    assert((lc_inodeGetDirtyPageCount(inode) == 0) || size);
    lc_initInodePageMarkers(inode);

    /* If the file still has dirty pages, set up first and last page
     * markers correctly.
     */
    if (size && lc_inodeGetDirtyPageCount(inode)) {

        /* Find the first page from the beginning */
        for (i = 0; i <= pg; i++) {
            dpage = lc_findDirtyPage(inode, i);
            if (dpage && dpage->dp_data) {
                lc_updateInodePageMarkers(inode, i);
                break;
            }
        }
        assert(lc_inodeGetFirstPage(inode) <= pg);

        /* Find the last page */
        for (i = pg; i >= 0; i--) {
            dpage = lc_findDirtyPage(inode, i);
            if (dpage && dpage->dp_data) {
                lc_updateInodePageMarkers(inode, i);
                break;
            }
        }
        assert(lc_inodeGetLastPage(inode) >= lc_inodeGetFirstPage(inode));
    }
    if (freed) {
        pcount = __sync_fetch_and_sub(&fs->fs_pcount, freed);
        assert(pcount >= freed);
        pcount = __sync_fetch_and_sub(&gfs->gfs_dcount, freed);
        assert(pcount >= freed);
    }
}

/* Truncate pages beyond the new size of the file */
void
lc_truncateFile(struct inode *inode, off_t size, bool remove) {
    uint64_t pg = size / LC_BLOCK_SIZE;
    struct gfs *gfs;
    struct fs *fs;
    bool zero;

    assert(S_ISREG(inode->i_mode));

    /* If nothing to truncate, return */
    if ((lc_inodeGetEmap(inode) == NULL) &&
        (lc_inodeGetDirtyPageCount(inode) == 0) &&
        (inode->i_extentLength == 0)) {
        assert(inode->i_page == NULL);
        assert(!(inode->i_flags & LC_INODE_SHARED));
        if (remove) {
            assert(inode->i_dinode.di_blocks == 0);
            inode->i_private = true;
        }
        return;
    }
    fs = inode->i_fs;
    gfs = fs->fs_gfs;

    /* Copy emap list before changing it */
    if (inode->i_flags & LC_INODE_SHARED) {
        if (size == 0) {

            /* Return after setting up these if truncating down to 0 */
            if (remove) {
                inode->i_dinode.di_blocks = 0;
                inode->i_extentBlock = 0;
                inode->i_extentLength = 0;
                inode->i_flags &= ~LC_INODE_SHARED;
                inode->i_private = true;
            }
            lc_inodeSetEmap(inode, NULL);
            lc_invalidatePages(gfs, fs, inode, size);
            return;
        }
        lc_copyEmap(gfs, fs, inode);
    }
    assert(!(inode->i_flags & LC_INODE_SHARED));

    /* Free blocks allocated beyond new eof */
    zero = lc_emapTruncate(gfs, fs, inode, size, pg, remove);

    if (size % LC_BLOCK_SIZE) {

        /* Adjust the last page if it is partially truncated */
        lc_truncatePage(gfs, fs, inode, pg, size % LC_BLOCK_SIZE, zero);
    }
    lc_invalidatePages(gfs, fs, inode, size);
}
