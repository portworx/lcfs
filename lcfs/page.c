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
    inode->i_fpage = ((inode->i_size + LC_BLOCK_SIZE - 1) / LC_BLOCK_SIZE) + 1;
    inode->i_lpage = 0;
}

/* Update page markers tracked with the inode */
static inline void
lc_updateInodePageMarkers(struct inode *inode, uint64_t pg) {
    if (inode->i_fpage > pg) {
        inode->i_fpage = pg;
    }
    if (inode->i_lpage < pg) {
        inode->i_lpage = pg;
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
    while (next) {

        /* Keep the list sorted */
        if (page > next->dh_pg) {
            break;
        }
        prev = &next->dh_next;
        next = next->dh_next;
    }
    hpage->dh_next = next;
    *prev = hpage;
}

/* Return the requested page if allocated already */
static inline struct dpage *
lc_findDirtyPage(struct inode *inode, uint64_t pg) {
    struct dhpage *dhpage;
    int hash;

    if (inode->i_flags & LC_INODE_HASHED) {
        assert(inode->i_pcount == 0);

        /* If the page is not between first and last pages, return */
        if ((inode->i_dpcount == 0) || (pg < inode->i_fpage) ||
            (pg > inode->i_lpage)) {
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
    return (pg < inode->i_pcount) ? &inode->i_page[pg] : NULL;
}

/* Flush dirty pages if the inode accumulated too many */
bool
lc_flushInodeDirtyPages(struct inode *inode, uint64_t page, bool unlock,
                        bool force) {
    struct dpage *dpage;

    /* Do not trigger flush if the last page is not fully filled up for a
     * sequentially written file.
     */
    if (!force && (inode->i_extentLength || (inode->i_emap == NULL))) {
        dpage = lc_findDirtyPage(inode, page);
        if ((dpage == NULL) ||
            (dpage->dp_data &&
             (dpage->dp_poffset || (dpage->dp_psize != LC_BLOCK_SIZE)))) {
            return false;
        }
    }
    lc_flushPages(inode->i_fs->fs_gfs, inode->i_fs, inode, false, unlock);
    return true;
}

/* Add inode to the layer dirty list if it is not already in it */
void
lc_addDirtyInode(struct fs *fs, struct inode *inode) {
    assert(S_ISREG(inode->i_mode));
    pthread_mutex_lock(&fs->fs_dilock);
    if ((inode->i_dnext == NULL) && (fs->fs_dirtyInodesLast != inode)) {
        if (fs->fs_dirtyInodesLast) {
            fs->fs_dirtyInodesLast->i_dnext = inode;
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
    struct inode *next = inode->i_dnext;

    assert(S_ISREG(inode->i_mode));
    if (prev) {
        prev->i_dnext = next;
    } else {
        assert(fs->fs_dirtyInodes == inode);
        fs->fs_dirtyInodes = next;
    }
    if (fs->fs_dirtyInodesLast == inode) {
        assert(prev || (fs->fs_dirtyInodes == NULL));
        fs->fs_dirtyInodesLast = prev;
    }
    inode->i_dnext = NULL;
    return next;
}

/* Flush inodes on the dirty list */
void
lc_flushDirtyInodeList(struct fs *fs, bool force) {
    struct inode *inode, *prev = NULL, *next;
    struct gfs *gfs = fs->fs_gfs;
    bool flushed;
    uint64_t id;

    if ((fs->fs_dirtyInodes == NULL) || fs->fs_removed) {
        return;
    }
    if (force) {
        pthread_mutex_lock(&fs->fs_dilock);
    } else if (pthread_mutex_trylock(&fs->fs_dilock)) {
        return;
    }

    /* Increment flusher id and store it with inodes flushed by this thread.
     * This helps to avoid processing same inodes over and over.
     */
    id = ++fs->fs_flusher;
    inode = fs->fs_dirtyInodes;
    while (inode && !fs->fs_removed) {
        assert(inode->i_fs == fs);

        /* Stop if remaining inodes are recently processed and not ready yet */
        if (inode->i_flusher >= id) {
            break;
        }
        inode->i_flusher = id;
        if (inode->i_flags & LC_INODE_REMOVED) {

            /* Take removed inodes off of the dirty list */
            inode = lc_removeDirtyInode(fs, inode, prev);
        } else if (!pthread_rwlock_trywrlock(&inode->i_rwlock)) {
            next = lc_removeDirtyInode(fs, inode, prev);
            if (inode->i_dpcount && !(inode->i_flags & LC_INODE_REMOVED) &&
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
                    if (inode->i_dpcount && (inode->i_ocount == 0)) {
                        lc_addDirtyInode(fs, inode);
                    }
                    pthread_rwlock_unlock(&inode->i_rwlock);
                }
            } else {
                pthread_rwlock_unlock(&inode->i_rwlock);
                inode = next;
                continue;
            }
            if (fs->fs_pcount < (LC_MAX_LAYER_DIRTYPAGES / 2)) {
                return;
            }
            if (force) {

                /* If memory is available now, wakeup any threads waiting for
                 * memory.
                 */
                if (lc_checkMemoryAvailable()) {
                    pthread_cond_broadcast(&gfs->gfs_mcond);
                }
                pthread_mutex_lock(&fs->fs_dilock);
            } else if (pthread_mutex_trylock(&fs->fs_dilock)) {
                return;
            }
            prev = NULL;
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
            uint64_t pg, struct extent **extents) {
    uint16_t poffset, psize, dsize, eof;
    struct page *bpage;
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
    struct dhpage *dhpage = NULL, **prev;
    struct fs *fs = inode->i_fs;
    struct dpage *page;
    char *pdata;
    int hash;

    assert(pg >= inode->i_fpage);
    assert(pg <= inode->i_lpage);
    assert(inode->i_dpcount);
    if (inode->i_flags & LC_INODE_HASHED) {
        assert(inode->i_pcount == 0);
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
        assert(pg < inode->i_pcount);
        page = &inode->i_page[pg];
    }
    pdata = page ? page->dp_data : NULL;
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

        /* XXX Adjust hash table size based on file size */
        dhpage = lc_malloc(fs, LC_DHASH_MIN * sizeof(struct dhpage *),
                           LC_MEMTYPE_DPAGEHASH);
        memset(dhpage, 0, LC_DHASH_MIN * sizeof(struct dhpage *));

        /* Move all dirty pages to the hash table */
        if (inode->i_pcount) {
            assert(inode->i_fpage < inode->i_pcount);
            assert(inode->i_lpage < inode->i_pcount);
            for (i = inode->i_fpage; i <= inode->i_lpage; i++) {
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

        /* Copy current page table and zero out rest of the entries */
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

/* Get current page dirty list filled up with valid data */
static char *
lc_getDirtyPage(struct gfs *gfs, struct inode *inode, uint64_t pg,
                struct extent **extents) {
    struct dpage *dpage;
    char *pdata;

    dpage = lc_findDirtyPage(inode, pg);
    pdata = dpage ? dpage->dp_data : NULL;
    if (pdata &&
        ((dpage->dp_poffset != 0) || (dpage->dp_psize != LC_BLOCK_SIZE))) {

        /* Fill up a partial page */
        lc_fillPage(gfs, inode, dpage, pg, extents);
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
    assert(!(inode->i_flags & LC_INODE_SHARED));
    assert((inode->i_flags & LC_INODE_HASHED) || (pg < inode->i_pcount));

    /* Check if the block is full of zeros */
    if ((poffset == 0) && (psize == LC_BLOCK_SIZE) &&
        !memcmp(data, gfs->gfs_zPage, LC_BLOCK_SIZE)) {
        data = gfs->gfs_zPage;
    }
    dpage = lc_findDirtyPage(inode, pg);
    if (dpage == NULL) {

        /* If no dirty page exists, add this one and return */
        assert(inode->i_flags & LC_INODE_HASHED);
        assert(inode->i_pcount == 0);
        lc_addDirtyPage(fs, inode->i_hpage, pg, data, poffset, psize);
        inode->i_dpcount++;
        lc_updateInodePageMarkers(inode, pg);
        if (data != gfs->gfs_zPage) {
            page->dp_data = NULL;
        }
        return 1;
    }

    /* If no dirty page exists, add the new page and return */
    if (dpage->dp_data == NULL) {
        assert(!(inode->i_flags & LC_INODE_HASHED));
        dpage->dp_data = data;
        dpage->dp_poffset = poffset;
        dpage->dp_psize = psize;
        inode->i_dpcount++;
        lc_updateInodePageMarkers(inode, pg);
        if (data != gfs->gfs_zPage) {
            page->dp_data = NULL;
        }
        return 1;
    }
    assert(inode->i_fpage <= pg);
    assert(inode->i_lpage >= pg);

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
    wsize = fuse_buf_copy(dst, bufv, FUSE_BUF_SPLICE_NONBLOCK);
    assert(wsize == size);
    return pcount;
}

/* Update pages of a file with provided data */
uint64_t
lc_addPages(struct inode *inode, off_t off, size_t size,
            struct dpage *dpages, uint64_t pcount) {
    uint64_t page = off / LC_BLOCK_SIZE, count = 0;
    struct extent *extent;
    struct fs *fs = inode->i_fs;
    struct gfs *gfs = fs->fs_gfs;
    off_t endoffset = off + size;
    struct dpage *dpage;
    uint64_t added = 0;

    assert(S_ISREG(inode->i_mode));

    /* Update inode size if needed */
    if (endoffset > inode->i_size) {
        inode->i_size = endoffset;
    }

    /* Skip the write if incoming data is fully zeroes and the file empty */
    if ((inode->i_dinode.di_blocks == 0) && (inode->i_dpcount == 0)) {
        while (count < pcount) {
            dpage = &dpages[count];
            if (memcmp(dpage->dp_data, gfs->gfs_zPage, dpage->dp_psize)) {
                break;
            }
            count++;
        }
        if (count == pcount) {
            return 0;
        }
        count = 0;
    }

    extent = inode->i_emap;
    lc_inodeAllocPages(inode);
    if (inode->i_dpcount == 0) {

        /* Initialize first and last page markers when the first dirty page
         * added.
         */
        lc_initInodePageMarkers(inode);
    }

    /* Link the dirty pages to the inode, merging with any existing ones */
    while (count < pcount) {
        dpage = &dpages[count];
        added += lc_mergePage(gfs, inode, page, dpage, &extent);

        /* Flush dirty pages if the inode accumulated too many */
        if ((inode->i_dpcount >= LC_MAX_FILE_DIRTYPAGES) &&
            !(inode->i_flags & LC_INODE_TMP)) {
            lc_flushInodeDirtyPages(inode, page, false, false);
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
    struct extent *extent = inode->i_emap;
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
              bool release, bool unlock) {
    uint64_t count = 0, bcount, start, end = 0, pstart = -1, zcount = 0;
    uint64_t lpage, pcount = 0, tcount = 0, rcount = 0, bstart = -1;
    struct page *page, *dpage = NULL, *tpage = NULL;
    uint64_t fcount = 0, block = LC_INVALID_BLOCK;
    struct extent *extents = NULL, *extent, *tmp;
    bool single, nocache;
    char *pdata;
    int64_t i;

    assert(S_ISREG(inode->i_mode));

    /* If inode does not have any pages, skip most of the work */
    if ((inode->i_dpcount == 0) || (inode->i_size == 0)) {
        goto out;
    }
    assert((inode->i_dinode.di_blocks == 0) ||
           (inode->i_extentLength == inode->i_dinode.di_blocks) ||
           inode->i_emap);
    lpage = (inode->i_size + LC_BLOCK_SIZE - 1) / LC_BLOCK_SIZE;
    assert((inode->i_flags & LC_INODE_HASHED) || (lpage < inode->i_pcount));
    bcount = inode->i_dpcount;
    start = inode->i_fpage;
    end = inode->i_lpage;
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

        /* Free any old blocks present */
        if (inode->i_extentLength) {
            lc_freeLayerDataBlocks(fs, inode->i_extentBlock,
                                   inode->i_extentLength, inode->i_private);
        } else if (inode->i_emap) {

            /* Traverse the extent list and free every extent */
            extent = inode->i_emap;
            while (extent) {
                assert(extent->ex_type == LC_EXTENT_EMAP);
                lc_validateExtent(gfs, extent);
                lc_addSpaceExtent(gfs, fs, &extents, lc_getExtentBlock(extent),
                                  lc_getExtentCount(extent), false);
                tmp = extent;
                extent = extent->ex_next;
                lc_free(fs, tmp, sizeof(struct extent), LC_MEMTYPE_EXTENT);
            }
            inode->i_emap = NULL;
        }
        inode->i_extentBlock = block;
        inode->i_extentLength = bcount;
        inode->i_dinode.di_blocks = bcount;
    } else if (inode->i_extentLength && (start == inode->i_extentLength) &&
               ((inode->i_extentBlock + inode->i_extentLength) == block) &&
               (bcount == rcount) && ((start + bcount - 1) == end)) {

        /* If previous extent is extended, keep the single extent layout */
        single = true;
        inode->i_extentLength += bcount;
        inode->i_dinode.di_blocks += bcount;
    } else if (inode->i_extentLength) {

        /* Expand from a single direct extent to an extent list */
        lc_expandEmap(gfs, fs, inode);
    }

    /* Invalidate pages if blocks are cached in kernel page cache */
    nocache = inode->i_private && !fs->fs_readOnly &&
              !(fs->fs_super->sb_flags & LC_SUPER_INIT);

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
        pdata = lc_removeDirtyPage(gfs, inode, i, false);
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
            page->p_nocache = nocache;
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
    assert(bcount == (tcount + fcount));
    assert(inode->i_dpcount == 0);

out:

    /* Free dirty page list as all pages are in block cache */
    if (release) {
        if (inode->i_flags & LC_INODE_HASHED) {
            lc_free(fs, inode->i_hpage, LC_DHASH_MIN * sizeof(struct dhpage *),
                    LC_MEMTYPE_DPAGEHASH);
            inode->i_hpage = NULL;
            inode->i_flags &= ~LC_INODE_HASHED;
        } else if (inode->i_pcount) {
            lc_free(fs, inode->i_page, inode->i_pcount * sizeof(struct dpage),
                    LC_MEMTYPE_DPAGEHASH);
            inode->i_page = NULL;
            inode->i_pcount = 0;
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
        lc_addPageForWriteBack(gfs, fs, dpage, tpage, tcount);
    }
    tcount += fcount;
    if (tcount) {
        pcount = __sync_fetch_and_sub(&fs->fs_pcount, tcount);
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
lc_truncatePage(struct fs *fs, struct inode *inode,
                uint64_t pg, uint16_t poffset) {

    struct dpage *dpage;

    lc_inodeAllocPages(inode);
    dpage = lc_findDirtyPage(inode, pg);
    if (dpage == NULL) {
        assert(inode->i_flags & LC_INODE_HASHED);
        lc_addDirtyPage(fs, inode->i_hpage, pg, NULL, 0, 0);
        dpage = lc_findDirtyPage(inode, pg);
    }

    /* Create a dirty page if one does not exist */
    if (dpage->dp_data == NULL) {
        lc_mallocBlockAligned(fs, (void **)&dpage->dp_data, LC_MEMTYPE_DATA);
        inode->i_dpcount++;
        lc_updateInodePageMarkers(inode, pg);
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
    uint64_t pg = size / LC_BLOCK_SIZE, lpage, freed, pcount;
    struct dhpage **next, *hpage;
    struct dpage *dpage;
    struct gfs *gfs;
    struct fs *fs;
    int64_t i;

    assert(S_ISREG(inode->i_mode));

    /* If nothing to truncate, return */
    if ((inode->i_emap == NULL) && (inode->i_dpcount == 0) &&
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
        assert(inode->i_dpcount == 0);
        assert(inode->i_page == NULL);
        if (size == 0) {

            /* Return after setting up these if truncating down to 0 */
            if (remove) {
                inode->i_dinode.di_blocks = 0;
                inode->i_extentBlock = 0;
                inode->i_extentLength = 0;
                inode->i_flags &= ~LC_INODE_SHARED;
                inode->i_private = true;
            }
            inode->i_emap = NULL;
            return;
        }
        lc_copyEmap(gfs, fs, inode);
    }
    assert(!(inode->i_flags & LC_INODE_SHARED));

    /* Free blocks allocated beyond new eof */
    lc_emapTruncate(gfs, fs, inode, size, pg, remove);

    if (size % LC_BLOCK_SIZE) {

        /* Adjust the last page if it is partially truncated */
        lc_truncatePage(fs, inode, pg, size % LC_BLOCK_SIZE);
    }
    if (size && (inode->i_dpcount == 0)) {
        return;
    }

    /* Remove dirty pages past the new size from the dirty list */
    freed = 0;
    if (inode->i_flags & LC_INODE_HASHED) {
        for (i = 0; (i < LC_DHASH_MIN) && inode->i_dpcount; i++) {
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
                assert(inode->i_dpcount > 0);
                inode->i_dpcount--;
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
    } else if (inode->i_pcount) {
        lpage = inode->i_lpage;
        assert(lpage < inode->i_pcount);

        /* Remove all pages after the new last page */
        for (i = pg; i <= lpage; i++) {
            dpage = lc_findDirtyPage(inode, i);
            if (dpage->dp_data == NULL) {
                continue;
            }

            /* Don't remove last paget if that is partially truncated */
            if ((i > pg) || ((size % LC_BLOCK_SIZE) == 0)) {
                lc_removeDirtyPage(gfs, inode, i, true);
                freed++;
            }
        }
        if (size == 0) {
            lc_free(fs, inode->i_page, inode->i_pcount * sizeof(struct dpage),
                    LC_MEMTYPE_DPAGEHASH);
            inode->i_page = NULL;
            inode->i_pcount = 0;
        }
    }
    assert((inode->i_dpcount == 0) || size);
    lc_initInodePageMarkers(inode);

    /* If the file still has dirty pages, set up first and last page
     * markers correctly.
     */
    if (size && inode->i_dpcount) {

        /* Find the first page from the beginning */
        for (i = 0; i <= pg; i++) {
            dpage = lc_findDirtyPage(inode, i);
            if (dpage && dpage->dp_data) {
                lc_updateInodePageMarkers(inode, i);
                break;
            }
        }
        assert(inode->i_fpage <= pg);

        /* Find the last page */
        for (i = pg; i >= 0; i--) {
            dpage = lc_findDirtyPage(inode, i);
            if (dpage && dpage->dp_data) {
                lc_updateInodePageMarkers(inode, i);
                break;
            }
        }
        assert(inode->i_lpage >= inode->i_fpage);
    }
    if (freed) {
        pcount = __sync_fetch_and_sub(&fs->fs_pcount, freed);
        assert(pcount >= freed);
    }
}
