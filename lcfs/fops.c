#include "includes.h"

#define LC_TIMEOUT_SEC  1.0

/* Initialize default values in fuse_entry_param structure.
 */
void
lc_epInit(struct fuse_entry_param *ep) {
    assert(ep->ino > LC_ROOT_INODE);
    ep->attr.st_ino = ep->ino;
    ep->generation = 1;
    ep->attr_timeout = LC_TIMEOUT_SEC;
    ep->entry_timeout = LC_TIMEOUT_SEC;
}

/* Create a new directory entry and associated inode */
static int
lc_createInode(struct fs *fs, ino_t parent, const char *name, mode_t mode,
               uid_t uid, gid_t gid, dev_t rdev, const char *target,
               struct fuse_file_info *fi, struct fuse_entry_param *ep) {
    struct gfs *gfs = fs->fs_gfs;
    struct inode *dir, *inode;
    ino_t ino;

    if (fs->fs_frozen) {
        lc_reportError(__func__, __LINE__, parent, EROFS);
        return EROFS;
    }

    /* Do not allow file creations in layer root directory */
    if (parent == gfs->gfs_layerRoot) {
        lc_reportError(__func__, __LINE__, parent, EPERM);
        return EPERM;
    }

    /* Do not allow new files when file system does not have much free space */
    if (!lc_hasSpace(gfs, false)) {
        lc_reportError(__func__, __LINE__, parent, ENOSPC);
        return ENOSPC;
    }
    dir = lc_getInode(fs, parent, NULL, true, true);
    if (dir == NULL) {
        lc_reportError(__func__, __LINE__, parent, ENOENT);
        return ENOENT;
    }
    assert(S_ISDIR(dir->i_mode));

    /* Clone the directory if needed */
    if (dir->i_flags & LC_INODE_SHARED) {
        lc_dirCopy(dir);
    }

    /* Get a new inode */
    inode = lc_inodeInit(fs, mode, uid, gid, rdev, parent, target);
    ino = inode->i_ino;

    /* Add the name and inode to the directory */
    lc_dirAdd(dir, ino, mode, name, strlen(name));
    if (S_ISDIR(mode)) {
        assert(inode->i_nlink >= 2);
        assert(dir->i_nlink >= 2);
        dir->i_nlink++;
    }
    lc_updateInodeTimes(dir, true, true);

    /* Identify files created in /tmp directory */
    if ((dir->i_flags & LC_INODE_TMP) ||
        (dir->i_ino == gfs->gfs_tmp_root)) {
        inode->i_flags |= LC_INODE_TMP;
    }
    lc_markInodeDirty(dir, LC_INODE_DIRDIRTY);
    lc_inodeUnlock(dir);
    lc_markInodeDirty(inode, 0);
    lc_copyStat(&ep->attr, inode);
    if (fi) {
        inode->i_ocount++;
        fi->fh = (uint64_t)inode;
    }
    lc_inodeUnlock(inode);
    ep->ino = lc_setHandle(fs->fs_gindex, ino);
    lc_epInit(ep);
    return 0;
}

/* Modify size of a file to the specified size */
static void
lc_truncate(struct inode *inode, off_t size, bool force) {
    assert(S_ISREG(inode->i_mode));

    /* Do not truncate a file if it was pulled from parent, but no data was
     * modified in it.  This file may be still in use.
     */
    if (!force && (inode->i_flags & LC_INODE_NOTRUNC)) {
        return;
    }
    if (size < inode->i_size) {

        /* Truncate pages/blocks beyond the new size */
        lc_truncateFile(inode, size, true);
    }
    assert(!(inode->i_flags & LC_INODE_SHARED));
    inode->i_size = size;
}

/* Removal of a directory */
static void
lc_removeDir(struct fs *fs, struct inode *dir) {
    assert(dir->i_size == 0);
    assert(dir->i_nlink == 2);
    dir->i_nlink = 0;
    if (dir->i_flags & LC_INODE_DHASHED) {
        lc_dirFreeHash(fs, dir);
    }
}

/* Remove an inode */
int
lc_removeInode(struct fs *fs, struct inode *dir, ino_t ino, bool rmdir,
               void **inodep) {
    bool removed = false, unlock = true;
    struct inode *inode;

    assert(S_ISDIR(dir->i_mode));

    /* Need to remove the inode only if it is already copied to this layer */
    inode = lc_getInode(fs, ino, NULL, false, true);
    if (inode == NULL) {
        lc_reportError(__func__, __LINE__, ino, ESTALE);
        return ESTALE;
    }
    if (inode->i_fs != fs) {

        /* Do not allow removing a directory not empty */
        if (S_ISDIR(inode->i_mode) && inode->i_size) {
            lc_inodeUnlock(inode);
            //lc_reportError(__func__, __LINE__, ino, EEXIST);
            return EEXIST;
        }
        lc_inodeUnlock(inode);
        return 0;
    }
    assert(inode->i_nlink);
    if (rmdir || S_ISDIR(inode->i_mode)) {
        assert(inode->i_parent == dir->i_ino);
        assert(S_ISDIR(inode->i_mode));

        /* Allow directory removals from the root file system even when
         * directories are not empty.  Skip this for target of a rename
         * operation.
         */
        if (inode->i_size && rmdir && (fs == lc_getGlobalFs(fs->fs_gfs))) {
            if (inodep) {

                /* Let caller do this processing after responding */
                *inodep = inode;
                unlock = false;
            } else {
                lc_removeTree(fs, inode);
            }
        }
        if (unlock) {
            if (inode->i_size) {

                /* Do not allow removing directories not empty */
                lc_inodeUnlock(inode);
                //lc_reportError(__func__, __LINE__, ino, EEXIST);
                return EEXIST;
            }
            lc_removeDir(fs, inode);
        }
        if (!rmdir) {
            assert(dir->i_nlink > 2);
            dir->i_nlink--;
        }
        inode->i_flags |= LC_INODE_REMOVED;
        removed = true;
    } else {
        if (inode->i_flags & LC_INODE_MLINKS) {
            lc_removeHlink(fs, inode, dir->i_ino);
        }
        inode->i_nlink--;

        /* Flag a file as removed on last unlink */
        if (inode->i_nlink == 0) {

            /* Truncate the file on last close */
            if ((inode->i_ocount == 0) && S_ISREG(inode->i_mode)) {
                if (inodep) {

                    /* Defer file truncation after responding in some cases */
                    *inodep = inode;
                    unlock = false;
                } else {
                    lc_truncate(inode, 0, true);
                }
            }
            inode->i_flags |= LC_INODE_REMOVED;
            removed = true;
        }
    }
    lc_markInodeDirty(inode, 0);
    if (removed) {
        if (!(inode->i_flags & LC_INODE_NOTRUNC)) {
            __sync_add_and_fetch(&fs->fs_ricount, 1);
        }
        __sync_sub_and_fetch(&fs->fs_gfs->gfs_super->sb_inodes, 1);
        lc_updateFtypeStats(fs, inode->i_mode, false);
    }
    if (unlock) {
        lc_inodeUnlock(inode);
    }
    return 0;
}

/* Remove a directory entry */
static int
lc_remove(struct fs *fs, ino_t parent, const char *name, void **inodep,
          bool rmdir) {
    struct inode *dir;
    int err;

    if (fs->fs_frozen) {
        lc_reportError(__func__, __LINE__, parent, EROFS);
        return EROFS;
    }
    dir = lc_getInode(fs, parent, NULL, true, true);
    if (dir == NULL) {
        lc_reportError(__func__, __LINE__, parent, ENOENT);
        return ENOENT;
    }
    assert(S_ISDIR(dir->i_mode));
    if (dir->i_flags & LC_INODE_SHARED) {
        lc_dirCopy(dir);
    }

    /* Lookup and remove the specified entry from the directory */
    err = lc_dirRemoveName(fs, dir, name, rmdir, inodep, false);
    lc_inodeUnlock(dir);
    if (err && (err != EEXIST)) {
        lc_reportError(__func__, __LINE__, parent, err);
    }
    return err;
}

/* Lookup the specified name in the specified directory */
static void
lc_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
    struct fuse_entry_param ep;
    struct fs *fs, *nfs = NULL;
    struct inode *inode, *dir;
    struct timeval start;
    int gindex, err = 0;
    ino_t ino;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, parent, 0, name);
    fs = lc_getLayerLocked(parent, false);
    dir = lc_getInode(fs, parent, NULL, false, false);
    if (dir == NULL) {
        lc_reportError(__func__, __LINE__, parent, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    ino = lc_dirLookup(fs, dir, name);
    if (ino == LC_INVALID_INODE) {
        lc_inodeUnlock(dir);

        /* Return fake inode info while looking up the diff path */
        if ((fs->fs_commitInProgress ||
             (fs->fs_super->sb_flags & LC_SUPER_INIT)) &&
            strstr(name, LC_COMMIT_TRIGGER_PREFIX)) {
            lc_copyFakeStat(&ep.attr);
            ep.ino = lc_setHandle(fs->fs_gindex, ep.attr.st_ino);
            lc_epInit(&ep);
            fuse_reply_entry(req, &ep);
            goto out;
        }

        /* Let kernel remember lookup failure as a negative entry */
        memset(&ep, 0, sizeof(struct fuse_entry_param));
        ep.entry_timeout = LC_TIMEOUT_SEC;
        fuse_reply_entry(req, &ep);
        err = ENOENT;
        goto out;
    }

    /* Check if looking up a layer root */
    if (parent == fs->fs_gfs->gfs_layerRoot) {

        /* Return the index of the actual layer */
        gindex = lc_getIndex(fs, parent, ino);
        if (fs->fs_gindex != gindex) {
            nfs = lc_getLayerLocked(lc_setHandle(gindex, ino), false);
        }
    } else {
        gindex = fs->fs_gindex;
    }
    inode = lc_getInode(nfs ? nfs : fs, ino, NULL, false, false);
    lc_inodeUnlock(dir);
    if (inode == NULL) {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
    } else {
        lc_copyStat(&ep.attr, inode);
        lc_inodeUnlock(inode);
        ep.ino = lc_setHandle(gindex, ino);
        lc_epInit(&ep);
        fuse_reply_entry(req, &ep);
    }

out:
    lc_statsAdd(nfs ? nfs : fs, LC_LOOKUP, err, &start);
    lc_unlock(fs);
    if (nfs) {
        lc_unlock(nfs);
    }
}

/* Get attributes of a file */
static void
lc_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct timeval start;
    struct inode *inode;
    struct stat stbuf;
    struct fs *fs;
    ino_t parent;
    int err = 0;

    lc_displayEntry(__func__, 0, ino, NULL);

    /* Check if the operation is on the fake inode */
    if ((lc_getInodeHandle(ino) == LC_COMMIT_TRIGGER_INODE) &&
        lc_getFsHandle(ino)) {
        lc_copyFakeStat(&stbuf);
        stbuf.st_ino = ino;
        fuse_reply_attr(req, &stbuf, LC_TIMEOUT_SEC);
        return;
    }
    lc_statsBegin(&start);
    fs = lc_getLayerLocked(ino, false);
    inode = lc_getInode(fs, ino, NULL, false, false);
    if (inode == NULL) {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    lc_copyStat(&stbuf, inode);
    parent = inode->i_parent;
    lc_inodeUnlock(inode);
    stbuf.st_ino = lc_setHandle(lc_getIndex(fs, parent, stbuf.st_ino),
                                stbuf.st_ino);
    fuse_reply_attr(req, &stbuf, LC_TIMEOUT_SEC);

out:
    lc_statsAdd(fs, LC_GETATTR, err, &start);
    lc_unlock(fs);
}

/* Change the attributes of the specified inode as requested */
static void
lc_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
            int to_set, struct fuse_file_info *fi) {
    bool ctime = false, mtime = false, flush = false, change;
    int err = 0, flags = 0, new_set;
    struct inode *inode, *handle;
    struct timeval start;
    struct stat stbuf;
    struct fs *fs;

    lc_displayEntry(__func__, ino, 0, NULL);

    /* Check if the operation is on the fake inode */
    if ((lc_getInodeHandle(ino) == LC_COMMIT_TRIGGER_INODE) &&
        lc_getFsHandle(ino)) {
        lc_copyFakeStat(&stbuf);
        stbuf.st_ino = ino;
        fuse_reply_attr(req, &stbuf, LC_TIMEOUT_SEC);
        return;
    }
    lc_statsBegin(&start);
    change = (to_set &
              (FUSE_SET_ATTR_MODE | FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID |
               FUSE_SET_ATTR_SIZE | FUSE_SET_ATTR_MTIME |
               FUSE_SET_ATTR_MTIME_NOW
#ifdef FUSE3
               | FUSE_SET_ATTR_CTIME
#endif
               ));
    fs = lc_getLayerLocked(ino, false);
    if (fs->fs_frozen) {
        lc_reportError(__func__, __LINE__, ino, EROFS);
        fuse_reply_err(req, EROFS);
        err = EROFS;
        goto out;
    }
    handle = fi ? (struct inode *)fi->fh : NULL;

    /* Check if uid/gid is really being changed */
    if (change && !(to_set & ~(FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID))) {
        new_set = to_set;
        inode = lc_getInode(fs, ino, handle, false, false);
        if (inode == NULL) {
            lc_reportError(__func__, __LINE__, ino, ENOENT);
            fuse_reply_err(req, ENOENT);
            err = ENOENT;
            goto out;
        }
        if ((to_set & FUSE_SET_ATTR_UID) &&
            (inode->i_dinode.di_uid == attr->st_uid)) {
            new_set &= ~FUSE_SET_ATTR_UID;
        }
        if ((to_set & FUSE_SET_ATTR_GID) &&
            (inode->i_dinode.di_gid == attr->st_gid)) {
            new_set &= ~FUSE_SET_ATTR_GID;
        }
        if (new_set == 0) {
            goto reply;
        }
        lc_inodeUnlock(inode);
    }
    inode = lc_getInode(fs, ino, handle, change, change);
    if (inode == NULL) {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }

    /* Change file permission */
    if (to_set & FUSE_SET_ATTR_MODE) {
        assert((inode->i_mode & S_IFMT) == (attr->st_mode & S_IFMT));
        inode->i_mode = attr->st_mode;
        ctime = true;
    }

    /* Change user id */
    if (to_set & FUSE_SET_ATTR_UID) {
        inode->i_dinode.di_uid = attr->st_uid;
        ctime = true;
    }

    /* Change group id */
    if (to_set & FUSE_SET_ATTR_GID) {
        inode->i_dinode.di_gid = attr->st_gid;
        ctime = true;
    }

    /* Modify file size */
    if (to_set & FUSE_SET_ATTR_SIZE) {
        flush = (attr->st_size < inode->i_size) &&
                inode->i_private && inode->i_dinode.di_blocks;
        lc_truncate(inode, attr->st_size, true);
        flags = LC_INODE_EMAPDIRTY;
        mtime = true;
        ctime = true;
    }

    /* Modify mtime */
    if (to_set & FUSE_SET_ATTR_MTIME) {
        inode->i_dinode.di_mtime = lc_statGetTime(attr, true);
        mtime = false;
    } else if (to_set & FUSE_SET_ATTR_MTIME_NOW) {

        /* Modify times to current time */
        mtime = true;
    }
#ifdef FUSE3

    /* Modify ctime */
    if (to_set & FUSE_SET_ATTR_CTIME) {
        inode->i_dinode.di_ctime = lc_statGetTime(attr, false);
        ctime = false;
    }
#endif
    if (ctime || mtime) {
        lc_updateInodeTimes(inode, mtime, ctime);
    }
    lc_markInodeDirty(inode, flags);

reply:
    lc_copyStat(&stbuf, inode);
    lc_inodeUnlock(inode);
    stbuf.st_ino = lc_setHandle(fs->fs_gindex, stbuf.st_ino);
    fuse_reply_attr(req, &stbuf, LC_TIMEOUT_SEC);

out:
    lc_statsAdd(fs, LC_SETATTR, err, &start);

    /* Queue a checkpoint if blocks are freed */
    if (flush && fs->fs_dpcount) {
        lc_layerChanged(fs->fs_gfs, false);
    }
    lc_unlock(fs);
}

/* Read target information for a symbolic link */
static void
lc_readlink(fuse_req_t req, fuse_ino_t ino) {
    char buf[LC_FILENAME_MAX + 1];
    struct timeval start;
    struct inode *inode;
    int size, err = 0;
    struct fs *fs;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, 0, ino, NULL);
    fs = lc_getLayerLocked(ino, false);
    inode = lc_getInode(fs, ino, NULL, false, false);
    if (inode == NULL) {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    assert(S_ISLNK(inode->i_mode));
    size = inode->i_size;
    assert(size && (size <= LC_FILENAME_MAX));
    strncpy(buf, inode->i_target, size);
    lc_inodeUnlock(inode);
    buf[size] = 0;
    fuse_reply_readlink(req, buf);

out:
    lc_statsAdd(fs, LC_READLINK, err, &start);
    lc_unlock(fs);
}

/* Create a special file */
static void
lc_mknod(fuse_req_t req, fuse_ino_t parent, const char *name,
          mode_t mode, dev_t rdev) {
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    struct fuse_entry_param e;
    struct timeval start;
    struct fs *fs;
    int err;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, parent, 0, name);
    fs = lc_getLayerLocked(parent, false);
    err = lc_createInode(fs, parent, name, mode & ~ctx->umask,
                         ctx->uid, ctx->gid, rdev, NULL, NULL, &e);
    if (err) {
        fuse_reply_err(req, err);
    } else {
        fuse_reply_entry(req, &e);
    }
    lc_statsAdd(fs, LC_MKNOD, err, &start);
    lc_unlock(fs);
}

/* Create a directory */
static void
lc_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode) {
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    struct fuse_entry_param e;
    struct timeval start;
    bool flush = false;
    struct gfs *gfs;
    struct fs *fs;
    int err;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, parent, 0, name);
    fs = lc_getLayerLocked(parent, false);
    err = lc_createInode(fs, parent, name, S_IFDIR | (mode & ~ctx->umask),
                         ctx->uid, ctx->gid, 0, NULL, NULL, &e);
    if (err) {
        fuse_reply_err(req, err);
    } else {

        /* Remember some special directories created */
        if (lc_getInodeHandle(parent) == LC_ROOT_INODE) {
            gfs = fs->fs_gfs;
            if (!gfs->gfs_layerRoot &&
                (strcmp(name, LC_LAYER_ROOT_DIR) == 0)) {
                lc_setLayerRoot(gfs, e.ino);
                flush = true;
            } else if (strcmp(name, LC_LAYER_TMP_DIR) == 0) {
                gfs->gfs_tmp_root = e.ino;
                printf("tmp root %ld\n", e.ino);
            }
        }
        fuse_reply_entry(req, &e);
    }
    lc_statsAdd(fs, LC_MKDIR, err, &start);
    lc_unlock(fs);
    if (flush) {

        /* Flush dirty pages created before starting layer management */
        lc_commitRoot(gfs, 0);
    }
}

/* Remove a file */
static void
lc_unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {
    struct inode *inode = NULL;
    struct timeval start;
    bool flush = false;
    struct fs *fs;
    int err;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, parent, 0, name);
    fs = lc_getLayerLocked(parent, false);
    err = lc_remove(fs, parent, name, (void **)&inode, false);
    fuse_reply_err(req, err);

    /* Free pages and blocks after responding */
    if (inode) {
        assert(inode->i_ocount == 0);
        flush = inode->i_private && inode->i_dinode.di_blocks;
        lc_truncate(inode, 0, false);
        lc_inodeUnlock(inode);
    }
    lc_statsAdd(fs, LC_UNLINK, err, &start);

    /* Queue a checkpoint if blocks are freed */
    if (flush && fs->fs_dpcount) {
        lc_layerChanged(fs->fs_gfs, false);
    }
    lc_unlock(fs);
}

/* Remove a directory */
static void
lc_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name) {
    struct inode *dir = NULL;
    struct timeval start;
    struct fs *fs;
    int err;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, parent, 0, name);
    fs = lc_getLayerLocked(parent, false);
    err = lc_remove(fs, parent, name, (void **)&dir, true);
    fuse_reply_err(req, err);
    if (dir) {

        /* Remove all files from the directory */
        assert(fs == lc_getGlobalFs(fs->fs_gfs));
        lc_removeTree(fs, dir);
        lc_removeDir(fs, dir);
        lc_inodeUnlock(dir);
    }
    lc_statsAdd(fs, LC_RMDIR, err, &start);
    lc_unlock(fs);
}

/* Create a symbolic link */
static void
lc_symlink(fuse_req_t req, const char *link, fuse_ino_t parent,
            const char *name) {
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    struct fuse_entry_param e;
    struct timeval start;
    struct fs *fs;
    int err;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, parent, 0, name);
    fs = lc_getLayerLocked(parent, false);
    err = lc_createInode(fs, parent, name, S_IFLNK | (0777 & ~ctx->umask),
                         ctx->uid, ctx->gid, 0, link, NULL, &e);
    if (err) {
        fuse_reply_err(req, err);
    } else {
        fuse_reply_entry(req, &e);
    }
    lc_statsAdd(fs, LC_SYMLINK, err, &start);
    lc_unlock(fs);
}

/* Detect json files created in root layer and use that as a trigger for
 * creating a checkpoint.
 */
static void
lc_checkJsonFile(struct fs *fs, const char *name) {
    int len = strlen(name);

    if ((len > LC_JSON_LENGTH) && ((name[0] == 'r') || (name[0] == 'c')) &&
        !strcmp(&name[len - LC_JSON_LENGTH], LC_JSON_EXTN)) {
        lc_layerChanged(fs->fs_gfs, false);
    }
}

/* Rename a file to another (mv) */
static void
lc_rename(fuse_req_t req, fuse_ino_t parent, const char *name,
           fuse_ino_t newparent, const char *newname
#ifdef FUSE3
           , unsigned int flags
#endif
           ) {
    bool tdirFirst = lc_getInodeHandle(parent) > lc_getInodeHandle(newparent);
    struct inode *inode, *sdir, *tdir = NULL;
    struct timeval start;
    struct fs *fs;
    int err = 0;
    ino_t ino;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, parent, newparent, name);
    fs = lc_getLayerLocked(parent, false);
    if (fs->fs_frozen) {
        lc_reportError(__func__, __LINE__, parent, EROFS);
        fuse_reply_err(req, EROFS);
        err = EROFS;
        goto out;
    }

    /* Follow some locking order while locking the directories */
    if (tdirFirst) {
        tdir = lc_getInode(fs, newparent, NULL, true, true);
        if (tdir == NULL) {
            lc_reportError(__func__, __LINE__, newparent, ENOENT);
            fuse_reply_err(req, ENOENT);
            err = ENOENT;
            goto out;
        }
        assert(S_ISDIR(tdir->i_mode));
    }
    sdir = lc_getInode(fs, parent, NULL, true, true);
    if (sdir == NULL) {
        if (tdir) {
            lc_inodeUnlock(tdir);
        }
        lc_reportError(__func__, __LINE__, parent, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    assert(S_ISDIR(sdir->i_mode));
    ino = lc_dirLookup(fs, sdir, name);
    if (ino == LC_INVALID_INODE) {
        lc_inodeUnlock(sdir);
        if (tdir) {
            lc_inodeUnlock(tdir);
        }
        lc_reportError(__func__, __LINE__, parent, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    assert(ino != newparent);
    if (sdir->i_flags & LC_INODE_SHARED) {
        lc_dirCopy(sdir);
    }
    if (!tdirFirst) {
        tdir = lc_getInode(fs, newparent, NULL, true, true);
        if (tdir == NULL) {
            lc_inodeUnlock(sdir);
            lc_reportError(__func__, __LINE__, newparent, ENOENT);
            fuse_reply_err(req, ENOENT);
            err = ENOENT;
            goto out;
        }
        assert(S_ISDIR(tdir->i_mode));
    }
    if (tdir && (tdir->i_flags & LC_INODE_SHARED)) {
        lc_dirCopy(tdir);
    }

    /* Need the inode if it is moved to a different directory */
    if (parent != newparent) {
        inode = lc_getInode(fs, ino, NULL, true, true);
        if (inode == NULL) {
            lc_inodeUnlock(sdir);
            lc_inodeUnlock(tdir);
            lc_reportError(__func__, __LINE__, ino, ENOENT);
            fuse_reply_err(req, ENOENT);
            err = ENOENT;
            goto out;
        }
    } else {
        inode = NULL;
    }

    /* Remove if target exists */
    err = lc_dirRemoveName(fs, tdir ? tdir : sdir, newname, false,
                           NULL, false);
    if (err && (err != ENOENT)) {

        /* Target is a non-empty directory */
        lc_inodeUnlock(sdir);
        if (tdir) {
            lc_inodeUnlock(tdir);
        }
        if (inode) {
            lc_inodeUnlock(inode);
        }
        lc_reportError(__func__, __LINE__, parent, err);
        fuse_reply_err(req, err);
        goto out;
    }
    fuse_reply_err(req, 0);

    /* Renaming to another directory */
    if (parent != newparent) {

        /* Add new name to the directory */
        lc_dirAdd(tdir, ino, inode->i_mode, newname, strlen(newname));

        /* Remove old name */
        lc_dirRemove(sdir, name);

        /* Adjust nlink if a directory is moved */
        if (S_ISDIR(inode->i_mode)) {
            assert(sdir->i_nlink > 2);
            sdir->i_nlink--;
            assert(tdir->i_nlink >= 2);
            tdir->i_nlink++;
        }
        lc_updateInodeTimes(tdir, true, true);
        lc_markInodeDirty(tdir, LC_INODE_DIRDIRTY);
        lc_inodeUnlock(tdir);
    } else {

        /* Rename within the directory */
        lc_dirRename(sdir, ino, name, newname);
    }
    lc_updateInodeTimes(sdir, true, true);
    lc_markInodeDirty(sdir, LC_INODE_DIRDIRTY);
    lc_inodeUnlock(sdir);
    if (inode) {
        if (inode->i_flags & LC_INODE_MLINKS) {
            lc_removeHlink(fs, inode, lc_getInodeHandle(parent));
            lc_addHlink(fs, inode, lc_getInodeHandle(newparent));
        } else {
            inode->i_parent = lc_getInodeHandle(newparent);
        }
        lc_updateInodeTimes(inode, false, true);
        lc_markInodeDirty(inode, 0);
        lc_inodeUnlock(inode);
    }
    err = 0;

out:
    lc_statsAdd(fs, LC_RENAME, err, &start);
    if ((err == 0) && (lc_getFsHandle(parent) == 0)) {
        lc_checkJsonFile(fs, newname);
    }
    lc_unlock(fs);
}

/* Create a new link to an inode */
static void
lc_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
         const char *newname) {
    struct fuse_entry_param ep;
    struct inode *inode, *dir;
    struct timeval start;
    struct fs *fs;
    int err = 0;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, newparent, ino, newname);
    fs = lc_getLayerLocked(ino, false);
    if (fs->fs_frozen) {
        lc_reportError(__func__, __LINE__, ino, EROFS);
        fuse_reply_err(req, EROFS);
        err = EROFS;
        goto out;
    }
    dir = lc_getInode(fs, newparent, NULL, true, true);
    if (dir == NULL) {
        lc_reportError(__func__, __LINE__, newparent, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    assert(S_ISDIR(dir->i_mode));
    assert(dir->i_nlink >= 2);
    if (dir->i_flags & LC_INODE_SHARED) {
        lc_dirCopy(dir);
    }
    inode = lc_getInode(fs, ino, NULL, true, true);
    if (inode == NULL) {
        lc_inodeUnlock(dir);
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    assert(!S_ISDIR(inode->i_mode));

    /* Add the newname to the directory */
    lc_dirAdd(dir, inode->i_ino, inode->i_mode, newname,
               strlen(newname));
    lc_updateInodeTimes(dir, true, true);
    lc_markInodeDirty(dir, LC_INODE_DIRDIRTY);
    lc_inodeUnlock(dir);

    /* Track hardlinks in the layer */
    lc_addHlink(fs, inode, lc_getInodeHandle(newparent));

    /* Increment link count of the inode */
    inode->i_nlink++;
    lc_updateInodeTimes(inode, false, true);
    lc_markInodeDirty(inode, 0);
    lc_copyStat(&ep.attr, inode);
    lc_inodeUnlock(inode);
    ep.ino = lc_setHandle(fs->fs_gindex, ino);
    lc_epInit(&ep);
    fuse_reply_entry(req, &ep);

out:
    lc_statsAdd(fs, LC_LINK, err, &start);
    lc_unlock(fs);
}

/* Set up file handle in case file is shared from another file system */
static int
lc_openInode(struct fs *fs, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct inode *inode;
    bool modify, trunc;

    fi->fh = 0;
    modify = (fi->flags & (O_WRONLY | O_RDWR));

    /* Do not allow modify operations in immutable layers */
    if (modify && fs->fs_frozen) {
        lc_reportError(__func__, __LINE__, ino, EROFS);
        return EROFS;
    }

    /* Check if file needs to be truncated on open and lock inode exclusive in
     * that case.
     */
    trunc = modify && (fi->flags & O_TRUNC);

    /* Clone the inode if opened for modification */
    inode = lc_getInode(fs, ino, NULL, trunc, trunc);
    if (inode == NULL) {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        return ENOENT;
    }

    /* Do not allow opening a removed inode */
    if (inode->i_flags & LC_INODE_REMOVED) {
        lc_inodeUnlock(inode);
        lc_reportError(__func__, __LINE__, ino, ESTALE);
        return ESTALE;
    }

    /* Increment open count if inode is private to this layer */
    if (inode->i_fs == fs) {
        if (trunc) {

            /* Truncate the file as requested */
            if (S_ISREG(inode->i_mode)) {
                lc_truncate(inode, 0, true);
            }
            inode->i_ocount++;
        } else {
            __sync_add_and_fetch(&inode->i_ocount, 1);
        }
    }
    lc_inodeUnlock(inode);
    fi->fh = (uint64_t)inode;

    /* Do not invalidate kernel page cache */
    fi->keep_cache = 1;

    /* XXX Cannot enable direct_io as that would break mmap. Need a new fuse
     * option for direct io for files which are not memory mapped.
     */
    //fi->direct_io = 1;
    return 0;
}

/* Decrement open count on an inode */
static void
lc_closeInode(struct fs *fs, struct inode *inode, struct fuse_file_info *fi,
              bool *inval) {
    bool reg = S_ISREG(inode->i_mode);

    /* Nothing to do if inode is not part of this layer */
    if (inode->i_fs != fs) {
        if (inval) {

            /* Invalidate pages in kernel page cache if multiple layers are
             * reading shared data from parent layer.
             * Allow a single container to cache data from parent layers in
             * kernel page cache.
             */
            *inval = reg && (inode->i_size > 0) && !fs->fs_parent->fs_single;
        }
        return;
    }

    /* Lock the inode exclusive and decrement open count */
    lc_inodeLock(inode, true);
    assert(inode->i_fs == fs);
    assert(inode->i_ocount > 0);
    inode->i_ocount--;

    /* Invalidate pages of shared files in kernel page cache */
    if (inval) {
        *inval = reg && (inode->i_ocount == 0) && (inode->i_size > 0) &&
                 (!inode->i_private || fs->fs_readOnly ||
                  (fs->fs_super->sb_flags & LC_SUPER_INIT));
    }

    /* Truncate a removed file on last close */
    if (reg && (inode->i_ocount == 0) && (inode->i_flags & LC_INODE_REMOVED)) {
        lc_truncate(inode, 0, false);
    }

    /* Flush dirty pages of a file on last close */
    if ((inode->i_ocount == 0) && (inode->i_flags & LC_INODE_EMAPDIRTY)) {
        assert(reg);
        if (fs->fs_readOnly || (fs->fs_super->sb_flags & LC_SUPER_INIT) ||
            ((fs->fs_gfs->gfs_layerRoot == 0) &&
             (fs->fs_gfs->gfs_dbIno != inode->i_ino))) {

            /* Inode emap needs to be stable before an inode could be cloned */
            lc_flushPages(fs->fs_gfs, fs, inode, true, true);
            return;
        } else if (!(inode->i_flags & (LC_INODE_REMOVED | LC_INODE_TMP)) &&
                   lc_inodeGetDirtyPageCount(inode)) {

            /* Add inode to dirty list of the layer */
            if (lc_inodeGetDirtyPageCount(inode) &&
                (lc_inodeGetDirtyNext(inode) == NULL) &&
                (fs->fs_dirtyInodesLast != inode)) {
                lc_addDirtyInode(fs, inode);
            }

            /* Flush pages of the inode if layer has too many dirty pages */
            if ((fs->fs_pcount >= LC_MAX_LAYER_DIRTYPAGES) &&
                lc_flushInodeDirtyPages(inode, inode->i_size / LC_BLOCK_SIZE,
                                        true, true)) {
                return;
            }
        }
    }
    lc_inodeUnlock(inode);
}

/* Open a file and return a handle corresponding to the inode number */
static void
lc_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct timeval start;
    struct fs *fs;
    bool inval;
    int err;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, 0, ino, NULL);
    fs = lc_getLayerLocked(ino, false);
    err = lc_openInode(fs, ino, fi);
    if (err) {
        fuse_reply_err(req, err);
    } else {
        err = fuse_reply_open(req, fi);
        if (err) {
            lc_closeInode(fs, (struct inode *)fi->fh, fi, &inval);
            if (inval) {
                lc_invalInodePages(fs->fs_gfs, ino);
            }
        }
    }
    lc_statsAdd(fs, LC_OPEN, err, &start);
    lc_unlock(fs);
}

/* Read from a file */
static void
lc_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
        struct fuse_file_info *fi) {
    struct fuse_bufvec *bufv;
    struct timeval start;
    struct inode *inode;
    struct page **pages;
    char **dbuf = NULL;
    off_t endoffset;
    uint64_t pcount;
    int i, err = 0;
    struct fs *fs;
    size_t fsize;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, ino, 0, NULL);

    /* Nothing to read with an empty buffer */
    if (size == 0) {
        fuse_reply_buf(req, NULL, 0);
        return;
    }
    pcount = (size / LC_BLOCK_SIZE) + 2;
    fsize = sizeof(struct fuse_bufvec) + (sizeof(struct fuse_buf) * pcount);
    bufv = alloca(fsize);
    pages = alloca(sizeof(struct page *) * pcount);
    memset(bufv, 0, fsize);
    fs = lc_getLayerLocked(ino, false);
    inode = lc_getInode(fs, ino, (struct inode *)fi->fh, false, false);
    if (inode == NULL) {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    assert(S_ISREG(inode->i_mode));

retry:
    /* Reading beyond file size is not allowed */
    fsize = inode->i_size;
    if (off >= fsize) {
        lc_inodeUnlock(inode);
        fuse_reply_buf(req, NULL, 0);
        goto out;
    }

    /* Check if end of read is past the size of the file */
    endoffset = off + size;
    if (endoffset > fsize) {
        endoffset = fsize;
    }
    err = lc_readFile(req, fs, inode, off, endoffset,
                      pcount, pages, dbuf, bufv);
    if (err) {

        /* Retry after allocating pages */
        assert(dbuf == NULL);
        dbuf = alloca(sizeof(char *) * pcount);
        for (i = 0; i < pcount; i++) {
            lc_mallocBlockAligned(fs, (void **)&dbuf[i], LC_MEMTYPE_DATA);
        }
        lc_inodeLock(inode, false);
        goto retry;
    }

out:
    lc_statsAdd(fs, LC_READ, err, &start);
    lc_waitMemory(false);
    lc_unlock(fs);
}

/* Flush a file */
static void
lc_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct inode *inode = (struct inode *)fi->fh;

    lc_displayEntry(__func__, ino, 0, NULL);
    fuse_reply_err(req, 0);
    if (inode) {
        lc_statsAdd(inode->i_fs, LC_FLUSH, 0, NULL);
    } else {
        assert(lc_getInodeHandle(ino) == LC_COMMIT_TRIGGER_INODE);
        assert(lc_getFsHandle(ino));
    }
}

/* Decrement open count on an inode */
static void
lc_releaseInode(fuse_req_t req, struct fs *fs, fuse_ino_t ino,
                struct fuse_file_info *fi, bool *inval) {
    struct inode *inode = (struct inode *)fi->fh;

    assert(fi);
    fuse_reply_err(req, 0);
    assert(inode->i_ino == lc_getInodeHandle(ino));
    lc_closeInode(fs, inode, fi, inval);
}

/* Release open count on a file */
static void
lc_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct gfs *gfs = getfs();
    struct timeval start;
    struct fs *fs;
    bool inval;

    lc_displayEntry(__func__, ino, 0, NULL);
    if ((struct inode *)fi->fh == NULL) {
        fuse_reply_err(req, 0);
        assert(lc_getInodeHandle(ino) == LC_COMMIT_TRIGGER_INODE);
        assert(lc_getFsHandle(ino));
        return;
    }
    lc_statsBegin(&start);
    fs = lc_getLayerLocked(ino, false);
    lc_releaseInode(req, fs, ino, fi, &inval);
    if (inval) {

        /* Invalidate kernel page cache if inode is sharing data with inodes in
         * other layers.
         */
        lc_invalInodePages(gfs, ino);
    }
    lc_statsAdd(fs, LC_RELEASE, false, &start);
    lc_unlock(fs);
}

/* Sync a file */
static void
lc_fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
          struct fuse_file_info *fi) {
    struct inode *inode = (struct inode *)fi->fh;

    /* Fsync is disabled in this file system as layers are made persistent when
     * needed.
     */
    lc_displayEntry(__func__, ino, 0, NULL);
    fuse_reply_err(req, 0);
    lc_statsAdd(inode->i_fs, LC_FSYNC, 0, NULL);
}

/* Open a directory */
static void
lc_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct timeval start;
    struct fs *fs;
    int err;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, ino, 0, NULL);
    fs = lc_getLayerLocked(ino, false);
    err = lc_openInode(fs, ino, fi);
    if (err) {
        fuse_reply_err(req, err);
    } else {
        err = fuse_reply_open(req, fi);
        if (err) {
            lc_closeInode(fs, (struct inode *)fi->fh, fi, NULL);
        }
    }
    lc_statsAdd(fs, LC_OPENDIR, err, &start);
    lc_unlock(fs);
}

/* Read entries from a directory */
static void
lc_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
            struct fuse_file_info *fi) {
    struct timeval start;
    struct inode *dir;
    struct stat st;
    struct fs *fs;
    int err = 0;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, ino, 0, NULL);
    memset(&st, 0, sizeof(struct stat));
    fs = lc_getLayerLocked(ino, false);
    dir = lc_getInode(fs, ino, (struct inode *)fi->fh, false, false);
    if (dir) {
        err = lc_dirReaddir(req, fs, dir, ino, size, off, &st);
        lc_inodeUnlock(dir);
    } else {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
    }
    lc_statsAdd(fs, LC_READDIR, err, &start);
    lc_unlock(fs);
}

/* Release a directory */
static void
lc_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct timeval start;
    struct fs *fs;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, ino, 0, NULL);
    fs = lc_getLayerLocked(ino, false);
    lc_releaseInode(req, fs, ino, fi, NULL);
    lc_statsAdd(fs, LC_RELEASEDIR, false, &start);
    lc_unlock(fs);
}

/* Sync a directory */
static void
lc_fsyncdir(fuse_req_t req, fuse_ino_t ino, int datasync,
             struct fuse_file_info *fi) {
    struct inode *inode = (struct inode *)fi->fh;

    /* Fsync is disabled in this file system as layers are made persistent when
     * needed.
     */
    lc_displayEntry(__func__, ino, 0, NULL);
    fuse_reply_err(req, 0);
    lc_statsAdd(inode->i_fs, LC_FSYNCDIR, 0, NULL);
}

/* File system statfs */
static void
lc_statfs(fuse_req_t req, fuse_ino_t ino) {
    struct gfs *gfs = getfs();
    struct super *super = gfs->gfs_super;
    struct timeval start;
    struct statvfs buf;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, ino, 0, NULL);
    memset(&buf, 0, sizeof(struct statvfs));
    buf.f_bsize = LC_BLOCK_SIZE;
    buf.f_frsize = LC_BLOCK_SIZE;
    buf.f_blocks = super->sb_tblocks;
    buf.f_bfree = buf.f_blocks - super->sb_blocks;
    buf.f_bavail = buf.f_bfree;
    buf.f_files = (fsfilcnt_t)(LC_FH_INODE - 1);
    buf.f_ffree = buf.f_files - super->sb_inodes;
    buf.f_favail = buf.f_ffree;
    buf.f_namemax = LC_FILENAME_MAX;
    fuse_reply_statfs(req, &buf);
    lc_statsAdd(lc_getGlobalFs(gfs), LC_STATFS, false, &start);
}

/* Set extended attributes on a file, currently used for creating a new file
 * system
 */
static void
lc_setxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
            const char *value, size_t size, int flags
#ifdef __APPLE__
            , uint32_t position) {
#else
             ) {
#endif
    lc_displayEntry(__func__, ino, 0, name);
    lc_xattrAdd(req, ino, name, value, size, flags);
}

/* Get extended attributes of the specified inode */
static void
lc_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name, size_t size
#ifdef __APPLE__
            , uint32_t position) {
#else
             ) {
#endif
    struct gfs *gfs = getfs();

    lc_displayEntry(__func__, ino, 0, name);

    /* Check if the request is for finding changes made in a layer */
    if ((ino == gfs->gfs_layerRoot) && (size == sizeof(uint64_t)) &&
        (lc_layerDiff(req, name, size) == 0)) {
        return;
    }

    /* If the file system does not have any extended attributes, return */
    if (!gfs->gfs_xattr_enabled) {
        //lc_reportError(__func__, __LINE__, ino, ENODATA);
        fuse_reply_err(req, ENODATA);
        return;
    }

    /* Take care of the special inode when commit is in progress */
    if ((lc_getInodeHandle(ino) == LC_COMMIT_TRIGGER_INODE) &&
        lc_getFsHandle(ino)) {
        fuse_reply_err(req, ENODATA);
        return;
    }

    /* XXX Figure out a way to avoid invoking this for system.posix_acl_access
     * and system.posix_acl_default.
     */
    lc_xattrGet(req, ino, name, size);
}

/* List extended attributes on a file */
static void
lc_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size) {
    struct gfs *gfs = getfs();

    lc_displayEntry(__func__, ino, 0, NULL);

    /* If the file system does not have any extended attributes, return */
    if (!gfs->gfs_xattr_enabled) {
        //lc_reportError(__func__, __LINE__, ino, ENODATA);
        if (size == 0) {
            fuse_reply_xattr(req, 0);
        } else {
            fuse_reply_err(req, ENODATA);
        }
        return;
    }
    lc_xattrList(req, ino, size);
}

/* Remove extended attributes */
static void
lc_removexattr(fuse_req_t req, fuse_ino_t ino, const char *name) {
    struct gfs *gfs = getfs();

    lc_displayEntry(__func__, ino, 0, name);

    /* If the file system does not have any extended attributes, return */
    if (!gfs->gfs_xattr_enabled) {
        //lc_reportError(__func__, __LINE__, ino, ENODATA);
        fuse_reply_err(req, ENODATA);
        return;
    }

    /* Take care of the special inode when commit is in progress */
    if ((lc_getInodeHandle(ino) == LC_COMMIT_TRIGGER_INODE) &&
        lc_getFsHandle(ino)) {
        fuse_reply_err(req, ENODATA);
        return;
    }
    lc_xattrRemove(req, ino, name);
}

/* Create a file */
static void
lc_create(fuse_req_t req, fuse_ino_t parent, const char *name,
          mode_t mode, struct fuse_file_info *fi) {
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    struct fuse_entry_param e;
    struct timeval start;
    struct fs *fs;
    int err;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, parent, 0, name);
    fs = lc_getLayerLocked(parent, false);

    /* Check if a layer commit is triggerd */
    if (fs->fs_parent && (lc_getInodeHandle(parent) == fs->fs_root) &&
        strstr(name, LC_COMMIT_TRIGGER_PREFIX)) {
        lc_commitLayer(req, fs, parent, name, fi);
        return;
    }
    err = lc_createInode(fs, parent, name, S_IFREG | (mode & ~ctx->umask),
                         ctx->uid, ctx->gid, 0, NULL, fi, &e);
    if (err) {
        fuse_reply_err(req, err);
    } else {

        /* Track file local_kv.db in root layer */
        if (!lc_getFsHandle(parent) && (parent != LC_ROOT_INODE) &&
            (fs->fs_gfs->gfs_dbIno == 0) &&
            (strcmp(name, LC_LAYER_LOCAL_KV_DB) == 0)) {
            fs->fs_gfs->gfs_dbIno = e.ino;
        }
        err = fuse_reply_create(req, &e, fi);
        if (err) {
            lc_closeInode(fs, (struct inode *)fi->fh, fi, NULL);
        }
    }
    lc_statsAdd(fs, LC_CREATE, err, &start);
    lc_unlock(fs);
}


/* IOCTLs for certain operations.  Supported only on layer root directory */
static void
lc_ioctl(fuse_req_t req, fuse_ino_t ino, int cmd, void *arg,
         struct fuse_file_info *fi, unsigned flags,
         const void *in_buf, size_t in_bufsz, size_t out_bufsz) {
    char name[in_bufsz + 1], *layer, *parent;
    struct gfs *gfs = getfs();
    int len, op;

    lc_displayEntry(__func__, ino, cmd, NULL);
    op = _IOC_NR(cmd);

    /* XXX For allowing graphdriver tests to run */
    if ((op == LAYER_CREATE) && (gfs->gfs_layerRoot != ino)) {
        lc_setLayerRoot(gfs, ino);
    }
    if (ino != gfs->gfs_layerRoot) {
        //lc_reportError(__func__, __LINE__, ino, ENOSYS);
        fuse_reply_err(req, ENOSYS);
        return;
    }
    if (in_bufsz > 0) {
        memcpy(name, in_buf, in_bufsz);
    }
    name[in_bufsz] = 0;
    switch (op) {
    case LAYER_CREATE:
    case LAYER_CREATE_RW:

        /* Check if parent is specified */
        len = _IOC_TYPE(cmd);
        if (len) {
            parent = name;
            name[len] = 0;
            layer = &name[len + 1];
        } else {
            parent = "";
            len = 0;
            layer = name;
        }
        lc_createLayer(req, gfs, layer, parent, len, op == LAYER_CREATE_RW);
        return;

    case LAYER_REMOVE:
        lc_deleteLayer(req, gfs, name);
        return;

    case LAYER_MOUNT:
    case LAYER_STAT:
    case LAYER_UMOUNT:
    case UMOUNT_ALL:
    case CLEAR_STAT:
        lc_layerIoctl(req, gfs, name, op);
        return;

    default:
        lc_reportError(__func__, __LINE__, ino, ENOSYS);
        fuse_reply_err(req, ENOSYS);
    }
}

/* Write provided data to file at the specified offset */
static void
lc_write_buf(fuse_req_t req, fuse_ino_t ino,
              struct fuse_bufvec *bufv, off_t off, struct fuse_file_info *fi) {
    uint64_t pcount, counted = 0, count = 0;
    struct fuse_bufvec *dst;
    struct timeval start;
    struct inode *inode;
    struct dpage *dpages;
    size_t size, wsize;
    struct gfs *gfs;
    struct fs *fs;
    int err = 0;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, ino, 0, NULL);
    size = bufv->buf[bufv->idx].size;
    pcount = (size / LC_BLOCK_SIZE) + 2;
    wsize = sizeof(struct fuse_bufvec) + (sizeof(struct fuse_buf) * pcount);
    dst = alloca(wsize);
    memset(dst, 0, wsize);
    dpages = alloca(pcount * sizeof(struct dpage));
    fs = lc_getLayerLocked(ino, false);
    gfs = fs->fs_gfs;
    if (fs->fs_frozen) {
        lc_reportError(__func__, __LINE__, ino, EROFS);
        fuse_reply_err(req, EROFS);
        err = EROFS;
        pcount = 0;
        goto out;
    }

    /* Make sure enough memory available before proceeding */
    lc_waitMemory(fs->fs_pcount > LC_MAX_LAYER_DIRTYPAGES);

    /* Copy in the data before taking the lock */
    pcount = lc_copyPages(fs, off, size, dpages, bufv, dst);
    counted = __sync_add_and_fetch(&fs->fs_pcount, pcount);
    __sync_add_and_fetch(&gfs->gfs_dcount, pcount);

    /* Check if file system has enough space for this write to proceed */
    if (!lc_hasSpace(fs->fs_gfs, false)) {
        lc_reportError(__func__, __LINE__, ino, ENOSPC);
        fuse_reply_err(req, ENOSPC);
        err = ENOSPC;
        goto out;
    }
    inode = lc_getInode(fs, ino, (struct inode *)fi->fh, true, true);
    if (inode == NULL) {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }

    /* Now the write cannot fail, so respond success */
    fuse_reply_write(req, size);
    assert(S_ISREG(inode->i_mode));

    /* Link the dirty pages to the inode and update times */
    count = lc_addPages(inode, off, size, dpages, pcount);
    assert(count <= pcount);
    lc_updateInodeTimes(inode, true, true);
    lc_markInodeDirty(inode, LC_INODE_EMAPDIRTY);
    lc_inodeUnlock(inode);

out:

    /* Adjust dirty page count if some pages existed before */
    if (counted && (pcount != count)) {
        count = pcount - count;
        counted = __sync_fetch_and_sub(&fs->fs_pcount, count);
        assert(counted >= count);
        counted = __sync_fetch_and_sub(&gfs->gfs_dcount, count);
        assert(counted >= count);
    }

    /* Release any pages not consumed */
    lc_freePages(fs, dpages, pcount);
    lc_statsAdd(fs, LC_WRITE_BUF, err, &start);

    /* Trigger flush of dirty pages if layer has too many now */
    if (!err && (fs->fs_pcount >= LC_MAX_LAYER_DIRTYPAGES)) {
        lc_flushDirtyInodeList(fs, false);
    }
    lc_unlock(fs);
}

#ifdef FUSE3
/* Readdir with file attributes */
static void
lc_readdirplus(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
               struct fuse_file_info *fi) {
    struct timeval start;
    struct inode *dir;
    struct fs *fs;
    int err = 0;

    lc_statsBegin(&start);
    lc_displayEntry(__func__, ino, 0, NULL);
    fs = lc_getLayerLocked(ino, false);
    dir = lc_getInode(fs, ino, (struct inode *)fi->fh, false, false);
    if (dir) {
        err = lc_dirReaddir(req, fs, dir, ino, size, off, NULL);
        lc_inodeUnlock(dir);
    } else {
        lc_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
    }
    lc_statsAdd(fs, LC_READDIRPLUS, err, &start);
    lc_unlock(fs);
}
#endif

/* Initialize a new file system */
static void
lc_init(void *userdata, struct fuse_conn_info *conn) {
    struct gfs *gfs = (struct gfs *)userdata;
    uint32_t count;

#ifdef FUSE3

    /* Use splice */
    conn->want |= FUSE_CAP_SPLICE_WRITE | FUSE_CAP_SPLICE_MOVE;

    /* Let kernel take care of setuid business */
    conn->want &= ~FUSE_CAP_HANDLE_KILLPRIV;
#else

    /* Need to support ioctls on directories */
    conn->want |= FUSE_CAP_IOCTL_DIR;
#endif
    count = __sync_add_and_fetch(&gfs->gfs_mcount, 1);
    if (count == LC_MAX_MOUNTS) {
#ifdef LC_PROFILING
        ProfilerStart("/tmp/lcfs");
#endif
    } else {
        pthread_mutex_lock(&gfs->gfs_lock);
        pthread_cond_signal(&gfs->gfs_mountCond);
        pthread_mutex_unlock(&gfs->gfs_lock);
    }
}

/* Unmount file system when both filesystems are unmounted */
static void
lc_destroy(void *fsp) {
    struct gfs *gfs = (struct gfs *)fsp;
    uint32_t count;

    count = __sync_sub_and_fetch(&gfs->gfs_mcount, 1);
    if (count == 0) {
#ifdef LC_PROFILING
        ProfilerStop();
#endif
        lc_unmount(gfs);
    }
}

/* Fuse operations registered with the fuse driver */
struct fuse_lowlevel_ops lc_ll_oper = {
    .init       = lc_init,
    .destroy    = lc_destroy,
    .lookup     = lc_lookup,
    //.forget     = lc_forget,
	.getattr	= lc_getattr,
    .setattr    = lc_setattr,
	.readlink	= lc_readlink,
	.mknod  	= lc_mknod,
	.mkdir  	= lc_mkdir,
	.unlink  	= lc_unlink,
	.rmdir		= lc_rmdir,
	.symlink	= lc_symlink,
    .rename     = lc_rename,
    .link       = lc_link,
    .open       = lc_open,
    .read       = lc_read,
    .flush      = lc_flush,
    .release    = lc_release,
    .fsync      = lc_fsync,
    .opendir    = lc_opendir,
    .readdir    = lc_readdir,
    .releasedir = lc_releasedir,
    .fsyncdir   = lc_fsyncdir,
    .statfs     = lc_statfs,
    .setxattr   = lc_setxattr,
    .getxattr   = lc_getxattr,
    .listxattr  = lc_listxattr,
    .removexattr  = lc_removexattr,
    //.access     = lc_access,
    .create     = lc_create,
#if 0
    .getlk      = lc_getlk,
    .setlk      = lc_setlk,
    .bmap       = lc_emap,
#endif
    .ioctl      = lc_ioctl,
#if 0
    .poll       = lc_poll,
#endif
    .write_buf  = lc_write_buf,
#if 0
    .retrieve_reply = lc_retrieve_reply,
    .forget_multi = lc_forget_multi,
    .flock      = lc_flock,
    .fallocate  = lc_fallocate,
#endif
#ifdef FUSE3
    .readdirplus = lc_readdirplus,
#endif
};
