#include "includes.h"

/* Add the specified extended attribute to the inode */
void
dfs_xattrAdd(fuse_req_t req, ino_t ino, const char *name,
             const char *value, size_t size, int flags) {
    struct gfs *gfs = getfs();
    int len = strlen(name);
    struct xattr *xattr;
    struct inode *inode;
    struct fs *fs;
    int err;

    fs = dfs_getfs(gfs, ino, false);
    inode = dfs_getInode(fs, ino, NULL, true, true);
    if (inode == NULL) {
        dfs_unlock(fs);
        dfs_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        return;
    }

    /* XXX Special case of creating a clone */
    if (inode->i_parent == gfs->gfs_snap_root) {
        dfs_inodeUnlock(inode);
        dfs_unlock(fs);
        err = dfs_newClone(gfs, ino, name);
        fuse_reply_err(req, err);
        return;
    }

    xattr = inode->i_xattr;
    while (xattr) {
        if (strcmp(name, xattr->x_name) == 0) {

            /* If XATTR_CREATE is specified, operation fails if an attribute
             * exists already.
             */
            if (flags == XATTR_CREATE) {
                dfs_inodeUnlock(inode);
                dfs_unlock(fs);
                dfs_reportError(__func__, __LINE__, ino, EEXIST);
                fuse_reply_err(req, EEXIST);
                return;
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
                dfs_inodeUnlock(inode);
                dfs_unlock(fs);
                fuse_reply_err(req, 0);
                return;
            }
        }
        xattr = xattr->x_next;
    }

    /* Operation fails if XATTR_CREATE is specified and attribute does not
     * exist.
     */
    if (flags == XATTR_REPLACE) {
        dfs_inodeUnlock(inode);
        dfs_unlock(fs);
        dfs_reportError(__func__, __LINE__, ino, ENODATA);
        fuse_reply_err(req, ENODATA);
        return;
    }
    xattr = malloc(sizeof(struct xattr));
    xattr->x_name = malloc(len + 1);
    strcpy(xattr->x_name, name);
    if (size) {
        xattr->x_value = malloc(size);
        memcpy(xattr->x_value, value, size);
    } else {
        xattr->x_value = NULL;
    }
    xattr->x_size = size;
    inode->i_xsize += len + 1;
    xattr->x_next = inode->i_xattr;
    inode->i_xattr = xattr;
    dfs_inodeUnlock(inode);
    dfs_unlock(fs);
    fuse_reply_err(req, 0);
}

/* Get the specified attribute of the inode */
void
dfs_xattrGet(fuse_req_t req, ino_t ino, const char *name,
             size_t size) {
    struct gfs *gfs = getfs();
    struct xattr *xattr;
    struct inode *inode;
    struct fs *fs;

    fs = dfs_getfs(gfs, ino, false);
    inode = dfs_getInode(fs, ino, NULL, false, false);
    if (inode == NULL) {
        dfs_unlock(fs);
        dfs_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        return;
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
            }
            dfs_inodeUnlock(inode);
            dfs_unlock(fs);
            return;
        }
        xattr = xattr->x_next;
    }
    dfs_inodeUnlock(inode);
    dfs_unlock(fs);
    fuse_reply_err(req, ENODATA);
}

/* List the specified attributes of the inode */
void
dfs_xattrList(fuse_req_t req, ino_t ino, size_t size) {
    struct gfs *gfs = getfs();
    struct xattr *xattr;
    struct inode *inode;
    struct fs *fs;
    char *buf;
    int i = 0;

    fs = dfs_getfs(gfs, ino, false);
    inode = dfs_getInode(fs, ino, NULL, false, false);
    if (inode == NULL) {
        dfs_unlock(fs);
        dfs_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        return;
    }
    if (size == 0) {
        fuse_reply_xattr(req, inode->i_xsize);
        dfs_inodeUnlock(inode);
        dfs_unlock(fs);
        return;
    } else if (size < inode->i_xsize) {
        dfs_unlock(fs);
        dfs_inodeUnlock(inode);
        dfs_reportError(__func__, __LINE__, ino, ERANGE);
        fuse_reply_err(req, ERANGE);
        return;
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
    dfs_unlock(fs);
    fuse_reply_buf(req, buf, inode->i_xsize);
}

/* Remove the specified extended attribute */
void
dfs_xattrRemove(fuse_req_t req, ino_t ino, const char *name) {
    struct xattr *xattr, *pxattr = NULL;
    struct gfs *gfs = getfs();
    struct inode *inode;
    struct fs *fs;
    int err;

    fs = dfs_getfs(gfs, ino, false);
    inode = dfs_getInode(fs, ino, NULL, true, true);
    if (inode == NULL) {
        dfs_unlock(fs);
        dfs_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        return;
    }

    /* XXX Special case of removing a clone */
    if (dfs_getInodeHandle(ino) == inode->i_parent) {
        dfs_inodeUnlock(inode);
        dfs_unlock(fs);
        err = dfs_removeClone(gfs, ino);
        fuse_reply_err(req, err);
        return;
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
            dfs_inodeUnlock(inode);
            dfs_unlock(fs);
            fuse_reply_err(req, 0);
            return;
        }
        pxattr = xattr;
        xattr = xattr->x_next;
    }
    dfs_inodeUnlock(inode);
    dfs_unlock(fs);
    dfs_reportError(__func__, __LINE__, ino, ENODATA);
    fuse_reply_err(req, ENODATA);
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
