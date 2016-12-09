#include "includes.h"

/* Link a new attribute to the inode */
static void
lc_xattrLink(struct inode *inode, const char *name, int len,
              const char *value, size_t size) {
    struct fs *fs = inode->i_fs;
    struct xattr *xattr = lc_malloc(fs, sizeof(struct xattr),
                                    LC_MEMTYPE_XATTR);

    xattr->x_name = lc_malloc(fs, len + 1, LC_MEMTYPE_XATTRNAME);
    memcpy(xattr->x_name, name, len);
    xattr->x_name[len] = 0;
    if (size) {
        xattr->x_value = lc_malloc(fs, size, LC_MEMTYPE_XATTRVALUE);
        memcpy(xattr->x_value, value, size);
    } else {
        xattr->x_value = NULL;
    }
    xattr->x_size = size;
    xattr->x_next = inode->i_xattr;
    inode->i_xattr = xattr;
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

    lc_statsBegin(&start);
    fs = lc_getfs(ino, false);
    if (fs->fs_snap) {
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

    if (!fs->fs_xattrEnabled) {
        gfs->gfs_xattr_enabled = true;
        fs->fs_xattrEnabled = true;
        lc_printf("Enabled extended attributes\n");
    }
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
                lc_markInodeDirty(inode, true, false, false, true);
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
    lc_markInodeDirty(inode, true, false, false, true);
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
    int err = 0;

    lc_statsBegin(&start);
    fs = lc_getfs(ino, false);
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
    xattr = inode->i_xattrData ? inode->i_xattr : NULL;
    while (xattr) {
        if (strcmp(name, xattr->x_name) == 0) {
            if (size == 0) {
                fuse_reply_xattr(req, xattr->x_size);
            } else if (size >= xattr->x_size) {
                fuse_reply_buf(req, xattr->x_value, xattr->x_size);
            } else {
                fuse_reply_err(req, ERANGE);
                err = ERANGE;
            }
            lc_inodeUnlock(inode);
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
    struct fs *fs;
    char *buf;

    lc_statsBegin(&start);
    fs = lc_getfs(ino, false);
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
    if (inode->i_xattrData == NULL) {
        fuse_reply_err(req, ENODATA);
        lc_inodeUnlock(inode);
        lc_reportError(__func__, __LINE__, ino, ENODATA);
        err = ENODATA;
        goto out;
    }
    if (size == 0) {
        fuse_reply_xattr(req, inode->i_xsize);
        lc_inodeUnlock(inode);
        goto out;
    }
    if (size < inode->i_xsize) {
        lc_inodeUnlock(inode);
        fuse_reply_err(req, ERANGE);
        lc_reportError(__func__, __LINE__, ino, ERANGE);
        err = ERANGE;
        goto out;
    }
    if (inode->i_xsize == 0) {
        lc_inodeUnlock(inode);
        fuse_reply_err(req, ENODATA);
        lc_reportError(__func__, __LINE__, ino, ENODATA);
        err = ENODATA;
        goto out;
    }
    buf = lc_malloc(fs, inode->i_xsize, LC_MEMTYPE_XATTRBUF);
    xattr = inode->i_xattr;
    while (xattr) {
        strcpy(&buf[i], xattr->x_name);
        i += strlen(xattr->x_name) + 1;
        xattr = xattr->x_next;
    }
    fuse_reply_buf(req, buf, inode->i_xsize);
    assert(i == inode->i_xsize);
    lc_inodeUnlock(inode);
    lc_free(fs, buf, inode->i_xsize, LC_MEMTYPE_XATTRBUF);

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
    struct xattr *xattr, *pxattr = NULL;
    struct timeval start;
    struct inode *inode;
    int err = 0, len;
    struct fs *fs;

    lc_statsBegin(&start);
    fs = lc_getfs(ino, false);
    if (fs->fs_snap) {
        lc_reportError(__func__, __LINE__, ino, EROFS);
        fuse_reply_err(req, EROFS);
        err = EROFS;
        goto out;
    }
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
            if (pxattr) {
                pxattr->x_next = xattr->x_next;
            } else {
                inode->i_xattr = xattr->x_next;
            }
            lc_freeXattr(fs, xattr);
            len = strlen(name) + 1;
            assert(inode->i_xsize >= len);
            inode->i_xsize -= len;
            lc_updateInodeTimes(inode, false, true);
            lc_markInodeDirty(inode, true, false, false, true);
            lc_inodeUnlock(inode);
            goto out;
        }
        pxattr = xattr;
        xattr = xattr->x_next;
    }
    fuse_reply_err(req, ENODATA);
    lc_inodeUnlock(inode);
    //lc_reportError(__func__, __LINE__, ino, ENODATA);
    err = ENODATA;

out:
    lc_statsAdd(fs, LC_REMOVEXATTR, err, &start);
    lc_unlock(fs);
}

/* Copy extended attributes of one inode to another */
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
    while (page) {
        count--;
        lc_addPageBlockHash(gfs, fs, page, block + count);
        xblock = (struct xblock *)page->p_data;
        xblock->xb_next = (page == fpage) ? LC_INVALID_BLOCK :
                                            block + count + 1;
        page = page->p_dnext;
    }
    assert(count == 0);
    lc_flushPageCluster(gfs, fs, fpage, pcount);
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
    while (xattr) {
        nsize = strlen(xattr->x_name);
        dsize = (2 * sizeof(uint16_t)) + nsize + xattr->x_size;
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
        if (!fs->fs_xattrEnabled) {
            gfs->gfs_xattr_enabled = true;
            fs->fs_xattrEnabled = true;
            lc_printf("Enabled extended attributes\n");
        }
        lc_xattrInit(fs, inode);
    }
    while (block != LC_INVALID_BLOCK) {
        lc_addSpaceExtent(gfs, fs, &inode->i_xattrExtents, block, 1);
        lc_readBlock(gfs, fs, block, xblock);
        xbuf = (char *)&xblock->xb_attr[0];
        remain = LC_BLOCK_SIZE - sizeof(struct xblock);
        while (remain > (2 * sizeof(uint16_t))) {
            dxattr = (struct dxattr *)xbuf;
            nsize = dxattr->dx_nsize;
            if (nsize == 0) {
                break;
            }
            lc_xattrLink(inode, dxattr->dx_nameValue, nsize,
                          &dxattr->dx_nameValue[nsize], dxattr->dx_nvalue);
            dsize = (2 * sizeof(uint16_t)) + nsize + dxattr->dx_nvalue;
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
    lc_blockFreeExtents(fs, inode->i_xattrExtents, false, false, true);
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
