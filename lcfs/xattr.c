#include "includes.h"

/* Link a new attribute to the inode */
static void
lc_xattrLink(struct inode *inode, const char *name, int len,
              const char *value, size_t size) {
    struct fs *fs = inode->i_fs;
    struct xattr *xattr = lc_malloc(fs, sizeof(struct xattr),
                                    LC_MEMTYPE_XATTR);

    assert(size < LC_BLOCK_SIZE);
    assert(len < LC_BLOCK_SIZE);
    xattr->x_name = lc_malloc(fs, len + 1, LC_MEMTYPE_XATTRNAME);
    memcpy(xattr->x_name, name, len);
    xattr->x_name[len] = 0;

    /* Check if value provided for the attribute */
    if (size) {
        xattr->x_value = lc_malloc(fs, size, LC_MEMTYPE_XATTRVALUE);
        memcpy(xattr->x_value, value, size);
    } else {
        xattr->x_value = NULL;
    }
    xattr->x_size = size;
    xattr->x_next = inode->i_xattr;
    inode->i_xattr = xattr;

    /* Keep track of total length for all attribute names */
    inode->i_xsize += len + 1;
}

/* Allocate xattr data for the inode */
static void
lc_xattrInit(struct fs *fs, struct inode *inode) {
    inode->i_xattrData = lc_malloc(fs, sizeof(struct ixattr),
                                   LC_MEMTYPE_XATTRINODE);
    memset(inode->i_xattrData, 0, sizeof(struct ixattr));
}

/* Add the specified extended attribute to the inode */
void
lc_xattrAdd(fuse_req_t req, ino_t ino, const char *name,
             const char *value, size_t size, int flags) {
    struct gfs *gfs = getfs();
    int len = strlen(name);
    struct timeval start;
    struct xattr *xattr;
    struct inode *inode;
    struct fs *fs;
    int err = 0;

    /* Do not allow creating extended attributes on the layer root directory */
    if (ino == gfs->gfs_layerRoot) {
        lc_reportError(__func__, __LINE__, ino, EPERM);
        fuse_reply_err(req, EPERM);
        return;
    }
    lc_statsBegin(&start);
    fs = lc_getLayerLocked(ino, false);
    if (fs->fs_child) {
        lc_reportError(__func__, __LINE__, ino, EROFS);
        fuse_reply_err(req, EROFS);
        err = EROFS;
        goto out;
    }
    inode = lc_getInode(fs, ino, NULL, true, true);
    if (inode == NULL) {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }

    /* If the file system does not have any extended attributes before, enable
     * that now.
     */
    if (!fs->fs_xattrEnabled) {
        gfs->gfs_xattr_enabled = true;
        fs->fs_xattrEnabled = true;
        lc_printf("Enabled extended attributes\n");
    }

    /* Initialize extended attributes for the inode if this is the first
     * extended attribute for the inode.
     */
    if (inode->i_xattrData == NULL) {
        lc_xattrInit(fs, inode);
    }
    xattr = inode->i_xattr;
    while (xattr) {
        if (strcmp(name, xattr->x_name) == 0) {

            /* If XATTR_CREATE is specified, operation fails if an attribute
             * exists already.
             */
            if (flags == XATTR_CREATE) {
                lc_inodeUnlock(inode);
                lc_reportError(__func__, __LINE__, ino, EEXIST);
                fuse_reply_err(req, EEXIST);
                err = EEXIST;
                goto out;
            } else {
                fuse_reply_err(req, 0);

                /* Replace the attribute with new value */
                if (xattr->x_value && (size != xattr->x_size)) {
                    lc_free(fs, xattr->x_value, xattr->x_size,
                            LC_MEMTYPE_XATTRVALUE);
                    xattr->x_value = NULL;
                }
                if (size) {
                    if (xattr->x_value == NULL) {
                        xattr->x_value = lc_malloc(fs, size,
                                                   LC_MEMTYPE_XATTRVALUE);
                    }
                    memcpy(xattr->x_value, value, size);
                }
                xattr->x_size = size;
                lc_updateInodeTimes(inode, false, true);
                lc_markInodeDirty(inode, LC_INODE_XATTRDIRTY);
                lc_inodeUnlock(inode);
                goto out;
            }
        }
        xattr = xattr->x_next;
    }

    /* Operation fails if XATTR_REPLACE is specified and attribute does not
     * exist.
     */
    if (flags == XATTR_REPLACE) {
        lc_inodeUnlock(inode);
        lc_reportError(__func__, __LINE__, ino, ENODATA);
        fuse_reply_err(req, ENODATA);
        err = ENODATA;
        goto out;
    }
    fuse_reply_err(req, 0);
    lc_xattrLink(inode, name, len, value, size);
    lc_updateInodeTimes(inode, false, true);
    lc_markInodeDirty(inode, LC_INODE_XATTRDIRTY);
    lc_inodeUnlock(inode);

out:
    lc_statsAdd(fs, LC_SETXATTR, err, &start);
    lc_unlock(fs);
}

/* Get the specified attribute of the inode */
void
lc_xattrGet(fuse_req_t req, ino_t ino, const char *name,
             size_t size) {
    struct timeval start;
    struct xattr *xattr;
    struct inode *inode;
    struct fs *fs;
    size_t xsize;
    int err = 0;

    lc_statsBegin(&start);
    fs = lc_getLayerLocked(ino, false);

    /* If the file system does not have any extended attributes, return without
     * looking up the inode.
     */
    if (!fs->fs_xattrEnabled) {
        fuse_reply_err(req, ENODATA);
        err = ENODATA;
        goto out;
    }
    inode = lc_getInode(fs, ino, NULL, false, false);
    if (inode == NULL) {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }

    /* Traverse the attribute list looking for the requested attribute */
    xattr = inode->i_xattrData ? inode->i_xattr : NULL;
    while (xattr) {
        if (strcmp(name, xattr->x_name) == 0) {
            xsize = xattr->x_size;
            if (size == 0) {
                lc_inodeUnlock(inode);

                /* If no buffer given, return the size of the attribute */
                fuse_reply_xattr(req, xsize);
            } else if (size >= xsize) {

                /* Respond with the attribute */
                fuse_reply_buf(req, xattr->x_value, xsize);
                lc_inodeUnlock(inode);
            } else {
                lc_inodeUnlock(inode);

                /* If attribute cannot fit in the buffer, return an error */
                fuse_reply_err(req, ERANGE);
                err = ERANGE;
            }
            goto out;
        }
        xattr = xattr->x_next;
    }
    lc_inodeUnlock(inode);
    fuse_reply_err(req, ENODATA);
    err = ENODATA;

out:
    lc_statsAdd(fs, LC_GETXATTR, err, &start);
    lc_unlock(fs);
}

/* List the specified attributes of the inode */
void
lc_xattrList(fuse_req_t req, ino_t ino, size_t size) {
    struct timeval start;
    struct xattr *xattr;
    struct inode *inode;
    int i = 0, err = 0;
    size_t xsize;
    struct fs *fs;
    char *buf;

    lc_statsBegin(&start);
    fs = lc_getLayerLocked(ino, false);

    /* If the file system does not have any extended attributes, return without
     * looking up the inode.
     */
    if (!fs->fs_xattrEnabled) {
        fuse_reply_err(req, ENODATA);
        err = ENODATA;
        goto out;
    }
    inode = lc_getInode(fs, ino, NULL, false, false);
    if (inode == NULL) {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }

    /* If checking the total size of attribute names, provide that info */
    xsize = inode->i_xattrData ? inode->i_xsize : 0;
    if (size == 0) {
        lc_inodeUnlock(inode);
        fuse_reply_xattr(req, xsize);
        goto out;
    }

    /* If inode does not have any extended attributes, return early */
    if (inode->i_xattrData == NULL) {
        lc_inodeUnlock(inode);
        fuse_reply_err(req, ENODATA);
        lc_reportError(__func__, __LINE__, ino, ENODATA);
        err = ENODATA;
        goto out;
    }

    /* If provided buffer is too small, return with ERANGE error */
    if (size < xsize) {
        lc_inodeUnlock(inode);
        fuse_reply_err(req, ERANGE);
        lc_reportError(__func__, __LINE__, ino, ERANGE);
        err = ERANGE;
        goto out;
    }

    /* If inode does not have any extended attributes, return early */
    if (xsize == 0) {
        lc_inodeUnlock(inode);
        fuse_reply_err(req, ENODATA);
        lc_reportError(__func__, __LINE__, ino, ENODATA);
        err = ENODATA;
        goto out;
    }

    /* Copy out the attributes */
    /* XXX Split the buffer into many iovs if there are too many attributes? */
    buf = lc_malloc(fs, xsize, LC_MEMTYPE_XATTRBUF);
    xattr = inode->i_xattr;
    while (xattr) {
        strcpy(&buf[i], xattr->x_name);
        i += strlen(xattr->x_name) + 1;
        xattr = xattr->x_next;
    }
    assert(i == xsize);
    lc_inodeUnlock(inode);
    fuse_reply_buf(req, buf, i);

    /* XXX Save the buffer for future use? */
    lc_free(fs, buf, i, LC_MEMTYPE_XATTRBUF);

out:
    lc_statsAdd(fs, LC_LISTXATTR, err, &start);
    lc_unlock(fs);
}

/* Free an xattr structure */
static inline void
lc_freeXattr(struct fs *fs, struct xattr *xattr) {
    if (xattr->x_value) {
        lc_free(fs, xattr->x_value, xattr->x_size, LC_MEMTYPE_XATTRVALUE);
    }
    lc_free(fs, xattr->x_name, strlen(xattr->x_name) + 1,
            LC_MEMTYPE_XATTRNAME);
    lc_free(fs, xattr, sizeof(struct xattr), LC_MEMTYPE_XATTR);
}

/* Remove the specified extended attribute */
void
lc_xattrRemove(fuse_req_t req, ino_t ino, const char *name) {
    struct xattr *xattr, **pxattr = NULL;
    struct timeval start;
    struct inode *inode;
    int err = 0, len;
    struct fs *fs;

    lc_statsBegin(&start);
    fs = lc_getLayerLocked(ino, false);
    if (fs->fs_child) {
        lc_reportError(__func__, __LINE__, ino, EROFS);
        fuse_reply_err(req, EROFS);
        err = EROFS;
        goto out;
    }

    /* If the file system does not have any extended attributes, return without
     * looking up the inode.
     */
    if (!fs->fs_xattrEnabled) {
        fuse_reply_err(req, ENODATA);
        err = ENODATA;
        goto out;
    }
    inode = lc_getInode(fs, ino, NULL, true, true);
    if (inode == NULL) {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }

    xattr = inode->i_xattrData ? inode->i_xattr : NULL;
    while (xattr) {
        if (strcmp(name, xattr->x_name) == 0) {
            fuse_reply_err(req, 0);
            if (pxattr == NULL) {
                pxattr = &inode->i_xattr;
            }
            *pxattr = xattr->x_next;
            lc_freeXattr(fs, xattr);
            len = strlen(name) + 1;
            assert(inode->i_xsize >= len);
            inode->i_xsize -= len;
            lc_updateInodeTimes(inode, false, true);
            lc_markInodeDirty(inode, LC_INODE_XATTRDIRTY);
            lc_inodeUnlock(inode);
            goto out;
        }
        pxattr = &xattr->x_next;
        xattr = xattr->x_next;
    }
    lc_inodeUnlock(inode);
    fuse_reply_err(req, ENODATA);
    //lc_reportError(__func__, __LINE__, ino, ENODATA);
    err = ENODATA;

out:
    lc_statsAdd(fs, LC_REMOVEXATTR, err, &start);
    lc_unlock(fs);
}

/* Copy extended attributes from parent inode */
bool
lc_xattrCopy(struct inode *inode, struct inode *parent) {
    struct fs *fs = inode->i_fs;
    struct xattr *xattr, *new;

    if (parent->i_xattrData == NULL) {
        return false;
    }
    assert(inode->i_xattrData == NULL);
    lc_xattrInit(fs, inode);
    xattr = parent->i_xattr;
    while (xattr) {
        new = lc_malloc(fs, sizeof(struct xattr), LC_MEMTYPE_XATTR);
        new->x_name = lc_malloc(fs, strlen(xattr->x_name) + 1,
                                LC_MEMTYPE_XATTRNAME);
        strcpy(new->x_name, xattr->x_name);
        if (xattr->x_value) {
            new->x_value = lc_malloc(fs, xattr->x_size, LC_MEMTYPE_XATTRVALUE);
            memcpy(new->x_value, xattr->x_value, xattr->x_size);
        }
        new->x_size = xattr->x_size;
        new->x_next = inode->i_xattr;
        inode->i_xattr = new;
        xattr = xattr->x_next;
    }
    inode->i_xsize = parent->i_xsize;
    return true;
}

/* Allocate a block and flush extended attributes */
static uint64_t
lc_xattrFlushBlocks(struct gfs *gfs, struct fs *fs,
                    struct page *fpage, uint64_t pcount) {
    uint64_t block, count = pcount;
    struct page *page = fpage;
    struct xblock *xblock;

    block = lc_blockAllocExact(fs, pcount, true, true);

    /* Link all the blocks together */
    while (page) {
        count--;
        lc_addPageBlockHash(gfs, fs, page, block + count);
        xblock = (struct xblock *)page->p_data;
        xblock->xb_magic = LC_XATTR_MAGIC;
        xblock->xb_next = (page == fpage) ? LC_INVALID_BLOCK :
                                            block + count + 1;
        lc_updateCRC(xblock, &xblock->xb_crc);
        page = page->p_dnext;
    }
    assert(count == 0);
    lc_flushPageCluster(gfs, fs, fpage, pcount, false);
    return block;
}

/* Add a new page to the list of extended attributes blocks */
static struct page *
lc_xattrAddPage(struct gfs *gfs, struct fs *fs, struct xblock *xblock,
                int remain, struct page *page) {
    char *buf;

    if (remain) {
        buf = (char *)xblock;
        memset(&buf[LC_BLOCK_SIZE - remain], 0, remain);
    }
    return lc_getPageNoBlock(gfs, fs, (char *)xblock, page);
}

/* Flush extended attributes */
void
lc_xattrFlush(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    uint64_t block = LC_INVALID_BLOCK, pcount = 0;
    struct xattr *xattr = inode->i_xattr;
    struct xblock *xblock = NULL;
    int remain = 0, dsize, nsize;
    size_t size = inode->i_xsize;
    struct page *page = NULL;
    struct dxattr *dxattr;
    char *xbuf = NULL;

    if (inode->i_flags & LC_INODE_REMOVED) {
        inode->i_flags &= ~LC_INODE_XATTRDIRTY;
        return;
    }

    /* Traverse extended attribute list and copy those to pages */
    while (xattr) {
        nsize = strlen(xattr->x_name);
        dsize = sizeof(struct dxattr) + nsize + xattr->x_size;
        if (remain < dsize) {
            if (xblock) {
                page = lc_xattrAddPage(gfs, fs, xblock, remain, page);
            }
            lc_mallocBlockAligned(fs->fs_rfs, (void **)&xblock,
                                  LC_MEMTYPE_DATA);
            xbuf = (char *)&xblock->xb_attr[0];
            remain = LC_BLOCK_SIZE - sizeof(struct xblock);
            pcount++;
        }
        dxattr = (struct dxattr *)xbuf;
        dxattr->dx_nsize = nsize;
        dxattr->dx_nvalue = xattr->x_size;
        memcpy(dxattr->dx_nameValue, xattr->x_name, nsize);
        if (xattr->x_size) {
            memcpy(&dxattr->dx_nameValue[nsize], xattr->x_value,
                   xattr->x_size);
        }
        xbuf += dsize;
        remain -= dsize;
        xattr = xattr->x_next;
        size -= nsize + 1;
    }
    if (xblock) {
        page = lc_xattrAddPage(gfs, fs, xblock, remain, page);
    }
    if (pcount) {
        block = lc_xattrFlushBlocks(gfs, fs, page, pcount);
        lc_replaceMetaBlocks(fs, &inode->i_xattrExtents, block, pcount);
    }
    assert(size == 0);

    /* Link extended attribute blocks from the inode */
    inode->i_xattrBlock = block;
    assert(inode->i_flags & LC_INODE_DIRTY);
    inode->i_flags &= ~LC_INODE_XATTRDIRTY;
}

/* Read extended attributes */
void
lc_xattrRead(struct gfs *gfs, struct fs *fs, struct inode *inode,
              void *buf) {
    uint64_t block = inode->i_xattrBlock;
    struct xblock *xblock = buf;
    int remain, dsize, nsize;
    struct dxattr *dxattr;
    char *xbuf;

    assert(inode->i_xattrData == NULL);
    if (block != LC_INVALID_BLOCK) {

        /* Enable extended attributes on the file system if not enabled already
         */
        if (!fs->fs_xattrEnabled) {
            gfs->gfs_xattr_enabled = true;
            fs->fs_xattrEnabled = true;
            lc_printf("Enabled extended attributes\n");
        }
        lc_xattrInit(fs, inode);
    }

    /* Read all extended attribute blocks linked from the inode */
    while (block != LC_INVALID_BLOCK) {
        lc_addSpaceExtent(gfs, fs, &inode->i_xattrExtents, block, 1, false);
        lc_readBlock(gfs, fs, block, xblock);
        lc_verifyBlock(xblock, &xblock->xb_crc);
        assert(xblock->xb_magic == LC_XATTR_MAGIC);
        xbuf = (char *)&xblock->xb_attr[0];
        remain = LC_BLOCK_SIZE - sizeof(struct xblock);

        /* Process all attributes from the block */
        while (remain > sizeof(struct dxattr)) {
            dxattr = (struct dxattr *)xbuf;
            nsize = dxattr->dx_nsize;
            if (nsize == 0) {
                break;
            }
            lc_xattrLink(inode, dxattr->dx_nameValue, nsize,
                          &dxattr->dx_nameValue[nsize], dxattr->dx_nvalue);
            dsize = sizeof(struct dxattr) + nsize + dxattr->dx_nvalue;
            xbuf += dsize;
            remain -= dsize;
        }
        block = xblock->xb_next;
    }
}

/* Free all the extended attributes of an inode */
void
lc_xattrFree(struct inode *inode) {
    struct fs *fs = inode->i_fs;
    struct xattr *xattr, *tmp;

    if (inode->i_xattrData == NULL) {
        return;
    }
    lc_blockFreeExtents(fs, inode->i_xattrExtents, 0);
    xattr = inode->i_xattr;
    while (xattr) {
        tmp = xattr;
        xattr = xattr->x_next;
        lc_freeXattr(fs, tmp);
    }
    lc_free(fs, inode->i_xattrData, sizeof(struct ixattr),
            LC_MEMTYPE_XATTRINODE);
    inode->i_xattrData = NULL;
}
