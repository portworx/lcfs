#include "includes.h"

/* Link a new attribute to the inode */
static void
dfs_xattrLink(struct inode *inode, const char *name, int len,
              const char *value, size_t size) {
    struct xattr *xattr = malloc(sizeof(struct xattr));

    xattr->x_name = malloc(len + 1);
    memcpy(xattr->x_name, name, len);
    xattr->x_name[len] = 0;
    if (size) {
        xattr->x_value = malloc(size);
        memcpy(xattr->x_value, value, size);
    } else {
        xattr->x_value = NULL;
    }
    xattr->x_size = size;
    xattr->x_next = inode->i_xattr;
    inode->i_xattr = xattr;
    inode->i_xsize += len + 1;
}

/* Add the specified extended attribute to the inode */
void
dfs_xattrAdd(fuse_req_t req, ino_t ino, const char *name,
             const char *value, size_t size, int flags) {
    struct gfs *gfs = getfs();
    int len = strlen(name);
    struct timeval start;
    struct xattr *xattr;
    struct inode *inode;
    struct fs *fs;
    int err = 0;

    dfs_statsBegin(&start);
    fs = dfs_getfs(ino, false);
    if (fs->fs_snap) {
        dfs_reportError(__func__, __LINE__, ino, EROFS);
        fuse_reply_err(req, EROFS);
        err = EROFS;
        goto out;
    }
    inode = dfs_getInode(fs, ino, NULL, true, true);
    if (inode == NULL) {
        dfs_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }

    if (!gfs->gfs_xattr_enabled) {
        gfs->gfs_xattr_enabled = true;
        dfs_printf("Enabled extended attributes\n");
    }
    xattr = inode->i_xattr;
    while (xattr) {
        if (strcmp(name, xattr->x_name) == 0) {

            /* If XATTR_CREATE is specified, operation fails if an attribute
             * exists already.
             */
            if (flags == XATTR_CREATE) {
                dfs_inodeUnlock(inode);
                dfs_reportError(__func__, __LINE__, ino, EEXIST);
                fuse_reply_err(req, EEXIST);
                err = EEXIST;
                goto out;
            } else {

                /* Replace the attribute with new value */
                assert(flags == XATTR_REPLACE);
                if (xattr->x_value) {
                    free(xattr->x_value);
                }
                if (size) {
                    xattr->x_value = malloc(size);
                    memcpy(xattr->x_value, value, size);
                } else {
                    xattr->x_value = NULL;
                }
                xattr->x_size = size;
                dfs_updateInodeTimes(inode, false, false, true);
                dfs_markInodeDirty(inode, true, false, false, true);
                dfs_inodeUnlock(inode);
                fuse_reply_err(req, 0);
                goto out;
            }
        }
        xattr = xattr->x_next;
    }

    /* Operation fails if XATTR_REPLACE is specified and attribute does not
     * exist.
     */
    if (flags == XATTR_REPLACE) {
        dfs_inodeUnlock(inode);
        dfs_reportError(__func__, __LINE__, ino, ENODATA);
        fuse_reply_err(req, ENODATA);
        err = ENODATA;
        goto out;
    }
    dfs_xattrLink(inode, name, len, value, size);
    dfs_updateInodeTimes(inode, false, false, true);
    dfs_markInodeDirty(inode, true, false, false, true);
    dfs_inodeUnlock(inode);
    fuse_reply_err(req, 0);

out:
    dfs_statsAdd(fs, DFS_SETXATTR, err, &start);
    dfs_unlock(fs);
}

/* Get the specified attribute of the inode */
void
dfs_xattrGet(fuse_req_t req, ino_t ino, const char *name,
             size_t size) {
    struct timeval start;
    struct xattr *xattr;
    struct inode *inode;
    struct fs *fs;
    int err = 0;

    dfs_statsBegin(&start);
    fs = dfs_getfs(ino, false);
    inode = dfs_getInode(fs, ino, NULL, false, false);
    if (inode == NULL) {
        dfs_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    xattr = inode->i_xattr;
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
            dfs_inodeUnlock(inode);
            goto out;
        }
        xattr = xattr->x_next;
    }
    dfs_inodeUnlock(inode);
    fuse_reply_err(req, ENODATA);
    err = ENODATA;

out:
    dfs_statsAdd(fs, DFS_GETXATTR, err, &start);
    dfs_unlock(fs);
}

/* List the specified attributes of the inode */
void
dfs_xattrList(fuse_req_t req, ino_t ino, size_t size) {
    struct timeval start;
    struct xattr *xattr;
    struct inode *inode;
    int i = 0, err = 0;
    struct fs *fs;
    char *buf;

    dfs_statsBegin(&start);
    fs = dfs_getfs(ino, false);
    inode = dfs_getInode(fs, ino, NULL, false, false);
    if (inode == NULL) {
        dfs_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    if (size == 0) {
        fuse_reply_xattr(req, inode->i_xsize);
        dfs_inodeUnlock(inode);
        goto out;
    } else if (size < inode->i_xsize) {
        dfs_inodeUnlock(inode);
        dfs_reportError(__func__, __LINE__, ino, ERANGE);
        fuse_reply_err(req, ERANGE);
        err = ERANGE;
        goto out;
    }
    buf = malloc(inode->i_xsize);
    xattr = inode->i_xattr;
    while (xattr) {
        strcpy(&buf[i], xattr->x_name);
        i += strlen(xattr->x_name) + 1;
        xattr = xattr->x_next;
    }
    assert(i == inode->i_xsize);
    dfs_inodeUnlock(inode);
    fuse_reply_buf(req, buf, inode->i_xsize);
    free(buf);

out:
    dfs_statsAdd(fs, DFS_LISTXATTR, err, &start);
    dfs_unlock(fs);
}

/* Remove the specified extended attribute */
void
dfs_xattrRemove(fuse_req_t req, ino_t ino, const char *name) {
    struct xattr *xattr, *pxattr = NULL;
    struct timeval start;
    struct inode *inode;
    struct fs *fs;
    int err = 0;

    dfs_statsBegin(&start);
    fs = dfs_getfs(ino, false);
    if (fs->fs_snap) {
        dfs_reportError(__func__, __LINE__, ino, EROFS);
        fuse_reply_err(req, EROFS);
        err = EROFS;
        goto out;
    }
    inode = dfs_getInode(fs, ino, NULL, true, true);
    if (inode == NULL) {
        dfs_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }

    xattr = inode->i_xattr;
    while (xattr) {
        if (strcmp(name, xattr->x_name) == 0) {
            if (pxattr) {
                pxattr->x_next = xattr->x_next;
            } else {
                inode->i_xattr = xattr->x_next;
            }
            free(xattr->x_name);
            if (xattr->x_value) {
                free(xattr->x_value);
            }
            free(xattr);
            dfs_updateInodeTimes(inode, false, false, true);
            dfs_markInodeDirty(inode, true, false, false, true);
            dfs_inodeUnlock(inode);
            fuse_reply_err(req, 0);
            goto out;
        }
        pxattr = xattr;
        xattr = xattr->x_next;
    }
    dfs_inodeUnlock(inode);
    //dfs_reportError(__func__, __LINE__, ino, ENODATA);
    fuse_reply_err(req, ENODATA);
    err = ENODATA;

out:
    dfs_statsAdd(fs, DFS_REMOVEXATTR, err, &start);
    dfs_unlock(fs);
}

/* Copy extended attributes of one inode to another */
void
dfs_xattrCopy(struct inode *inode, struct inode *parent) {
    struct xattr *xattr = parent->i_xattr, *new;

    while (xattr) {
        new = malloc(sizeof(struct xattr));
        new->x_name = malloc(strlen(xattr->x_name) + 1);
        strcpy(new->x_name, xattr->x_name);
        if (xattr->x_value) {
            new->x_value = malloc(xattr->x_size);
            memcpy(new->x_value, xattr->x_value, xattr->x_size);
        }
        new->x_size = xattr->x_size;
        new->x_next = inode->i_xattr;
        inode->i_xattr = new;
        xattr = xattr->x_next;
    }
    inode->i_xsize = parent->i_xsize;
    inode->i_xattrdirty = true;
}

/* Flush extended attribute block */
static uint64_t
dfs_xattrFlushBlock(struct gfs *gfs, struct fs *fs, struct xblock *xblock,
                    int remain) {
    uint64_t block = dfs_blockAlloc(fs, 1);
    char *buf;

    dfs_printf("Writing out extended attr block %ld\n", block);
    if (remain) {
        buf = (char *)xblock;
        memset(&buf[DFS_BLOCK_SIZE - remain], 0, remain);
    }
    dfs_writeBlock(gfs->gfs_fd, xblock, block);
    return block;
}


/* Flush extended attributes */
void
dfs_xattrFlush(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    struct xattr *xattr = inode->i_xattr;
    uint64_t block = DFS_INVALID_BLOCK;
    struct xblock *xblock = NULL;
    int remain = 0, dsize, nsize;
    size_t size = inode->i_xsize;
    struct dxattr *dxattr;
    char *xbuf = NULL;

    //dfs_printf("Flushing extended attributes of inode %ld xsize %ld\n", inode->i_stat.st_ino, inode->i_xsize);
    if (inode->i_removed) {
        inode->i_xattrdirty = false;
        return;
    }
    while (xattr) {
        nsize = strlen(xattr->x_name);
        dsize = (2 * sizeof(uint16_t)) + nsize + xattr->x_size;
        if (remain < dsize) {
            if (xblock) {
                block = dfs_xattrFlushBlock(gfs, fs, xblock, remain);
            } else {
                posix_memalign((void **)&xblock, DFS_BLOCK_SIZE,
                               DFS_BLOCK_SIZE);
            }
            xblock->xb_next = block;
            xbuf = (char *)&xblock->xb_attr[0];
            remain = DFS_BLOCK_SIZE - sizeof(struct xblock);
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
        block = dfs_xattrFlushBlock(gfs, fs, xblock, remain);
        free(xblock);
    }
    assert(size == 0);

    /* XXX Free previously used blocks */
    inode->i_xattrBlock = block;
    inode->i_xattrdirty = false;
    inode->i_dirty = true;
}

/* Read extended attributes */
void
dfs_xattrRead(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    uint64_t block = inode->i_xattrBlock;
    int remain, dsize, nsize;
    struct xblock *xblock;
    struct dxattr *dxattr;
    char *xbuf;

    while (block != DFS_INVALID_BLOCK) {
        //dfs_printf("Reading extended attr block %ld\n", block);
        xblock = dfs_readBlock(gfs->gfs_fd, block);
        xbuf = (char *)&xblock->xb_attr[0];
        remain = DFS_BLOCK_SIZE - sizeof(struct xblock);
        while (remain > (2 * sizeof(uint16_t))) {
            dxattr = (struct dxattr *)xbuf;
            nsize = dxattr->dx_nsize;
            if (nsize == 0) {
                break;
            }
            dfs_xattrLink(inode, dxattr->dx_nameValue, nsize,
                          &dxattr->dx_nameValue[nsize], dxattr->dx_nvalue);
            dsize = (2 * sizeof(uint16_t)) + nsize + dxattr->dx_nvalue;
            xbuf += dsize;
            remain -= dsize;
        }
        block = xblock->xb_next;
        free(xblock);
    }
}

/* Free all the extended attributes of an inode */
void
dfs_xattrFree(struct inode *inode) {
    struct xattr *xattr = inode->i_xattr, *tmp;

    while (xattr) {
        tmp = xattr;
        xattr = xattr->x_next;
        free(tmp->x_name);
        if (tmp->x_value) {
            free(tmp->x_value);
        }
        free(tmp);
    }
}
