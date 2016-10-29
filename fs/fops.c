#include "includes.h"

/* XXX Check return values from fuse_reply_*, for example on interrupt */
/* XXX Do we need to track lookup counts and forget? */

/* Initialize default values in fuse_entry_param structure.
 */
static void
dfs_epInit(struct fuse_entry_param *ep) {
    assert(ep->ino > DFS_ROOT_INODE);
    ep->attr.st_ino = ep->ino;
    ep->generation = 1;
    ep->attr_timeout = 1.0;
    ep->entry_timeout = 1.0;
}

/* Create a new directory entry and associated inode */
static int
create(struct fs *fs, ino_t parent, const char *name, mode_t mode,
       uid_t uid, gid_t gid, dev_t rdev, const char *target,
       struct fuse_file_info *fi, struct fuse_entry_param *ep) {
    struct inode *dir, *inode;
    ino_t ino;

    if (fs->fs_snap) {
        dfs_reportError(__func__, __LINE__, parent, EROFS);
        return EROFS;
    }
    dir = dfs_getInode(fs, parent, NULL, true, true);
    if (dir == NULL) {
        dfs_reportError(__func__, __LINE__, parent, ENOENT);
        return ENOENT;
    }
    assert(S_ISDIR(dir->i_stat.st_mode));
    inode = dfs_inodeInit(fs, mode, uid, gid, rdev, parent, target);
    ino = inode->i_stat.st_ino;
    dfs_dirAdd(dir, ino, mode, name);
    if (S_ISDIR(mode)) {
        assert(inode->i_stat.st_nlink >= 2);
        assert(dir->i_stat.st_nlink >= 2);
        dir->i_stat.st_nlink++;
    }
    dfs_updateInodeTimes(dir, false, true, true);
    memcpy(&ep->attr, &inode->i_stat, sizeof(struct stat));
    if (fi) {
        inode->i_ocount++;
        fi->fh = (uint64_t)inode;
    }
    dfs_inodeUnlock(inode);
    dfs_inodeUnlock(dir);
    ep->ino = dfs_setHandle(fs->fs_gindex, ino);
    dfs_epInit(ep);
    return 0;
}

/* Truncate a file */
static void
dfs_truncate(struct inode *inode, off_t size) {
    int count;

    assert(S_ISREG(inode->i_stat.st_mode));
    if (size < inode->i_stat.st_size) {
        count = dfs_truncPages(inode, size);
        if (count) {
            dfs_blockFree(getfs(), count);
        }
    }
    inode->i_stat.st_size = size;
    dfs_inodeAllocPages(inode);
}

/* Remove a directory entry */
int
dremove(struct fs *fs, struct inode *dir, const char *name,
        ino_t ino, bool rmdir) {
    struct inode * inode = dfs_getInode(fs, ino, NULL, true, true);
    struct gfs *gfs;
    ino_t parent;
    int err = 0;

    if (inode == NULL) {
        dfs_reportError(__func__, __LINE__, ino, ESTALE);
        err = ESTALE;
        goto out;
    }
    assert(inode->i_stat.st_nlink);
    if (rmdir) {
        assert(dir->i_stat.st_nlink > 2);
        parent =  dir->i_stat.st_ino;
        assert(inode->i_parent == parent);
        gfs = fs->fs_gfs;

        /* Allow docker to remove some special directories while not empty */
        if ((inode->i_dirent != NULL) &&
            (dir->i_removed || (gfs->gfs_containers_root == parent) ||
             (gfs->gfs_tmp_root == parent) ||
             (gfs->gfs_mounts_root == parent) ||
             (gfs->gfs_sha256_root == parent))) {
            dfs_removeTree(fs, inode);
            /* XXX Does VFS deal with invalidating the entries under this
             * directory?
             */
        }
        if (inode->i_dirent != NULL) {
            dfs_inodeUnlock(inode);
            //dfs_reportError(__func__, __LINE__, ino, EEXIST);
            return EEXIST;
        }
        dir->i_stat.st_nlink--;
        assert(inode->i_stat.st_nlink == 2);
        inode->i_removed = true;
    } else {
        assert(dir->i_stat.st_nlink >= 2);
        inode->i_stat.st_nlink--;

        /* Flag a file as removed on last unlink */
        if (inode->i_stat.st_nlink == 0) {
            inode->i_removed = true;
            if ((inode->i_ocount == 0) && S_ISREG(inode->i_stat.st_mode)) {
                dfs_truncate(inode, 0);
            }
        }
    }

out:
    dfs_dirRemove(dir, name);
    if (inode) {
        dfs_updateInodeTimes(dir, false, false, true);
        dfs_inodeUnlock(inode);
    }
    return err;
}

/* Remove a directory entry */
static int
dfs_remove(struct fs *fs, ino_t parent, const char *name, bool rmdir) {
    struct inode *dir;
    int err = 0;
    ino_t ino;

    if (fs->fs_snap) {
        dfs_reportError(__func__, __LINE__, parent, EROFS);
        return EROFS;
    }
    dir = dfs_getInode(fs, parent, NULL, true, true);
    if (dir == NULL) {
        dfs_reportError(__func__, __LINE__, parent, ENOENT);
        return ENOENT;
    }
    assert(S_ISDIR(dir->i_stat.st_mode));
    /* XXX Combine lookup and removal */
    ino = dfs_dirLookup(fs, dir, name);
    if (ino == DFS_INVALID_INODE) {
        dfs_reportError(__func__, __LINE__, parent, ESTALE);
        err = ESTALE;
    } else {
        if (rmdir && (fs->fs_gindex == 0)) {

            /* Do not allow removing a snapshot root */
            if (dfs_getIndex(fs, parent, ino)) {
                dfs_reportError(__func__, __LINE__, parent, EEXIST);
                err = EEXIST;
            }
        }
        if (!err) {
            err = dremove(fs, dir, name, ino, rmdir);
        }
    }
    dfs_inodeUnlock(dir);
    return err;
}

/* Lookup the specified name in the specified directory */
static void
dfs_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
    struct fuse_entry_param ep;
    struct inode *inode, *dir;
    struct fs *fs, *nfs = NULL;
    struct timeval start;
    int gindex, err = 0;
    ino_t ino;

    dfs_statsBegin(&start);
    dfs_displayEntry(__func__, parent, 0, name);
    fs = dfs_getfs(parent, false);
    dir = dfs_getInode(fs, parent, NULL, false, false);
    if (dir == NULL) {
        dfs_reportError(__func__, __LINE__, parent, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    ino = dfs_dirLookup(fs, dir, name);
    if (ino == DFS_INVALID_INODE) {
        dfs_inodeUnlock(dir);

        /* Let kernel remember lookup failure as a negative entry */
        memset(&ep, 0, sizeof(struct fuse_entry_param));
        ep.entry_timeout = 1.0;
        fuse_reply_entry(req, &ep);
        err = ENOENT;
        goto out;
    }

    /* Check if looking up a snapshot root */
    if (parent == fs->fs_gfs->gfs_snap_root) {
        gindex = dfs_getIndex(fs, parent, ino);
        if (fs->fs_gindex != gindex) {
            nfs = dfs_getfs(dfs_setHandle(gindex, ino), false);
        }
    } else {
        gindex = fs->fs_gindex;
    }
    inode = dfs_getInode(nfs ? nfs : fs, ino, NULL, false, false);
    dfs_inodeUnlock(dir);
    if (inode == NULL) {
        dfs_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
    } else {
        memcpy(&ep.attr, &inode->i_stat, sizeof(struct stat));
        dfs_inodeUnlock(inode);
        ep.ino = dfs_setHandle(gindex, ino);
        dfs_epInit(&ep);
        fuse_reply_entry(req, &ep);
    }

out:
    dfs_statsAdd(nfs ? nfs : fs, DFS_LOOKUP, err, &start);
    dfs_unlock(fs);
    if (nfs) {
        dfs_unlock(nfs);
    }
}

#if 0
/* Forget an inode - not relevant for this file system */
static void
dfs_forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup) {
    struct timeval start;

    dfs_statsBegin(&start);
    dfs_displayEntry(__func__, 0, ino, NULL);
    fuse_reply_none(req);
    dfs_statsAdd(fs, DFS_FORGET, err, &start);
}
#endif

/* Get attributes of a file */
static void
dfs_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct timeval start;
    struct inode *inode;
    struct stat stbuf;
    struct fs *fs;
    ino_t parent;
    int err = 0;

    dfs_statsBegin(&start);
    dfs_displayEntry(__func__, 0, ino, NULL);
    fs = dfs_getfs(ino, false);
    inode = dfs_getInode(fs, ino, NULL, false, false);
    if (inode == NULL) {
        dfs_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    memcpy(&stbuf, &inode->i_stat, sizeof(struct stat));
    parent = inode->i_parent;
    dfs_inodeUnlock(inode);
    stbuf.st_ino = dfs_setHandle(dfs_getIndex(fs, parent,
                                              stbuf.st_ino), stbuf.st_ino);
    fuse_reply_attr(req, &stbuf, 1.0);

out:
    dfs_statsAdd(fs, DFS_GETATTR, err, &start);
    dfs_unlock(fs);
}

/* Change the attributes of the specified inode as requested */
static void
dfs_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
            int to_set, struct fuse_file_info *fi) {
    bool ctime = false, mtime = false, atime = false;
    struct inode *inode, *handle;
    struct timeval start;
    struct stat stbuf;
    struct fs *fs;
    int err = 0;

    dfs_statsBegin(&start);
    dfs_displayEntry(__func__, ino, 0, NULL);
    fs = dfs_getfs(ino, false);
    if (fs->fs_snap) {
        dfs_reportError(__func__, __LINE__, ino, EROFS);
        fuse_reply_err(req, EROFS);
        err = EROFS;
        goto out;
    }
    handle = fi ? (struct inode *)fi->fh : NULL;
    inode = dfs_getInode(fs, ino, handle, true, true);
    if (inode == NULL) {
        dfs_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    if (to_set & FUSE_SET_ATTR_MODE) {
        assert((inode->i_stat.st_mode & S_IFMT) == (attr->st_mode & S_IFMT));
        inode->i_stat.st_mode = attr->st_mode;
        ctime = true;
    }
    if (to_set & FUSE_SET_ATTR_UID) {
        inode->i_stat.st_uid = attr->st_uid;
        ctime = true;
    }
    if (to_set & FUSE_SET_ATTR_GID) {
        inode->i_stat.st_gid = attr->st_gid;
        ctime = true;
    }
    if (to_set & FUSE_SET_ATTR_SIZE) {
        dfs_truncate(inode, attr->st_size);
        mtime = true;
        ctime = true;
    }
    if (to_set & FUSE_SET_ATTR_ATIME) {
        inode->i_stat.st_atime = attr->st_atime;
        atime = false;
    } else if (to_set & FUSE_SET_ATTR_ATIME_NOW) {
        atime = true;
    }
    if (to_set & FUSE_SET_ATTR_MTIME) {
        inode->i_stat.st_mtime = attr->st_mtime;
        mtime = false;
    } else if (to_set & FUSE_SET_ATTR_MTIME_NOW) {
        mtime = true;
        ctime = true;
    }
    if (ctime || mtime || atime) {
        dfs_updateInodeTimes(inode, atime, mtime, ctime);
    }
    memcpy(&stbuf, &inode->i_stat, sizeof(struct stat));
    dfs_inodeUnlock(inode);
    stbuf.st_ino = dfs_setHandle(fs->fs_gindex, stbuf.st_ino);
    fuse_reply_attr(req, &stbuf, 1.0);

out:
    dfs_statsAdd(fs, DFS_SETATTR, err, &start);
    dfs_unlock(fs);
}

/* Read target information for a symbolic link */
static void
dfs_readlink(fuse_req_t req, fuse_ino_t ino) {
    char buf[DFS_FILENAME_MAX + 1];
    struct timeval start;
    struct inode *inode;
    int size, err = 0;
    struct fs *fs;

    dfs_statsBegin(&start);
    dfs_displayEntry(__func__, 0, ino, NULL);
    fs = dfs_getfs(ino, false);
    inode = dfs_getInode(fs, ino, NULL, false, false);
    if (inode == NULL) {
        dfs_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    assert(S_ISLNK(inode->i_stat.st_mode));
    size = inode->i_stat.st_size;
    assert(size && (size <= DFS_FILENAME_MAX));
    strncpy(buf, inode->i_target, size);
    dfs_inodeUnlock(inode);
    buf[size] = 0;
    fuse_reply_readlink(req, buf);

out:
    dfs_statsAdd(fs, DFS_READLINK, err, &start);
    dfs_unlock(fs);
}

/* Create a special file */
static void
dfs_mknod(fuse_req_t req, fuse_ino_t parent, const char *name,
          mode_t mode, dev_t rdev) {
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    struct fuse_entry_param e;
    struct timeval start;
    struct fs *fs;
    int err;

    dfs_statsBegin(&start);
    dfs_displayEntry(__func__, parent, 0, name);
    fs = dfs_getfs(parent, false);
    err = create(fs, parent, name, mode & ~ctx->umask,
                 ctx->uid, ctx->gid, rdev, NULL, NULL, &e);
    if (err) {
        fuse_reply_err(req, err);
    } else {
        fuse_reply_entry(req, &e);
    }
    dfs_statsAdd(fs, DFS_MKNOD, err, &start);
    dfs_unlock(fs);
}

/* Create a directory */
static void
dfs_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode) {
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    struct fuse_entry_param e;
    struct timeval start;
    struct gfs *gfs;
    struct fs *fs;
    bool global;
    int err;

    dfs_statsBegin(&start);
    dfs_displayEntry(__func__, parent, 0, name);
    fs = dfs_getfs(parent, false);
    err = create(fs, parent, name, S_IFDIR | (mode & ~ctx->umask),
                 ctx->uid, ctx->gid, 0, NULL, NULL, &e);
    if (err) {
        fuse_reply_err(req, err);
    } else {
        fuse_reply_entry(req, &e);

        /* Remember some special directories created */
        gfs = getfs();
        global = dfs_getInodeHandle(parent) == DFS_ROOT_INODE;
        if (global && (strcmp(name, "dfs") == 0)) {
            gfs->gfs_snap_rootInode = dfs_getInode(dfs_getGlobalFs(gfs), e.ino,
                                                   NULL, false, false);
            dfs_inodeUnlock(gfs->gfs_snap_rootInode);
            gfs->gfs_snap_root = e.ino;
            printf("snapshot root inode %ld\n", e.ino);
        } else if (global && (strcmp(name, "containers") == 0)) {
            gfs->gfs_containers_root = e.ino;
            printf("containers root %ld\n", e.ino);
        } else if (global && (strcmp(name, "tmp") == 0)) {
            gfs->gfs_tmp_root = e.ino;
            printf("tmp root %ld\n", e.ino);
        } else if ((parent == gfs->gfs_layerdb_root) &&
                   (strcmp(name, "mounts") == 0)) {
            assert(gfs->gfs_mounts_root == 0);
            gfs->gfs_mounts_root = e.ino;
            printf("mounts root %ld\n", e.ino);
        } else if ((parent == gfs->gfs_layerdb_root) &&
                   (strcmp(name, "sha256") == 0)) {
            assert(gfs->gfs_sha256_root == 0);
            gfs->gfs_sha256_root = e.ino;
            printf("sha256 root %ld\n", e.ino);
        }
    }
    dfs_statsAdd(fs, DFS_MKDIR, err, &start);
    dfs_unlock(fs);
}

/* Remove a file */
static void
dfs_unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {
    struct timeval start;
    struct fs *fs;
    int err;

    dfs_statsBegin(&start);
    dfs_displayEntry(__func__, parent, 0, name);
    fs = dfs_getfs(parent, false);
    err = dfs_remove(fs, parent, name, false);
    fuse_reply_err(req, err);
    dfs_statsAdd(fs, DFS_UNLINK, err, &start);
    dfs_unlock(fs);
}

/* Remove a special directory */
static void
dfs_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name) {
    struct timeval start;
    struct fs *fs;
    int err;

    dfs_statsBegin(&start);
    dfs_displayEntry(__func__, parent, 0, name);
    fs = dfs_getfs(parent, false);
    err = dfs_remove(fs, parent, name, true);
    fuse_reply_err(req, err);
    dfs_statsAdd(fs, DFS_RMDIR, err, &start);
    dfs_unlock(fs);
}

/* Create a symbolic link */
static void
dfs_symlink(fuse_req_t req, const char *link, fuse_ino_t parent,
            const char *name) {
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    struct fuse_entry_param e;
    struct timeval start;
    struct fs *fs;
    int err;

    dfs_statsBegin(&start);
    dfs_displayEntry(__func__, parent, 0, name);
    fs = dfs_getfs(parent, false);
    err = create(fs, parent, name, S_IFLNK | (0777 & ~ctx->umask),
                 ctx->uid, ctx->gid, 0, link, NULL, &e);
    if (err) {
        fuse_reply_err(req, err);
    } else {
        fuse_reply_entry(req, &e);
    }
    dfs_statsAdd(fs, DFS_SYMLINK, err, &start);
    dfs_unlock(fs);
}

/* Rename a file to another (mv) */
static void
dfs_rename(fuse_req_t req, fuse_ino_t parent, const char *name,
           fuse_ino_t newparent, const char *newname) {
    struct inode *inode, *sdir, *tdir = NULL;
    struct timeval start;
    ino_t ino, target;
    struct fs *fs;
    int err = 0;

    dfs_statsBegin(&start);
    dfs_displayEntry(__func__, parent, newparent, name);
    fs = dfs_getfs(parent, false);
    if (fs->fs_snap) {
        dfs_reportError(__func__, __LINE__, parent, EROFS);
        fuse_reply_err(req, EROFS);
        err = EROFS;
        goto out;
    }

    /* Follow some locking order while locking the directories */
    if (parent > newparent) {
        tdir = dfs_getInode(fs, newparent, NULL, true, true);
        if (tdir == NULL) {
            dfs_reportError(__func__, __LINE__, newparent, ENOENT);
            fuse_reply_err(req, ENOENT);
            err = ENOENT;
            goto out;
        }
        assert(S_ISDIR(tdir->i_stat.st_mode));
    }
    sdir = dfs_getInode(fs, parent, NULL, true, true);
    if (sdir == NULL) {
        if (tdir) {
            dfs_inodeUnlock(tdir);
        }
        dfs_reportError(__func__, __LINE__, parent, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    assert(S_ISDIR(sdir->i_stat.st_mode));
    if (parent < newparent) {
        tdir = dfs_getInode(fs, newparent, NULL, true, true);
        if (tdir == NULL) {
            dfs_inodeUnlock(sdir);
            dfs_reportError(__func__, __LINE__, newparent, ENOENT);
            fuse_reply_err(req, ENOENT);
            err = ENOENT;
            goto out;
        }
        assert(S_ISDIR(tdir->i_stat.st_mode));
    }
    ino = dfs_dirLookup(fs, sdir, name);
    if (ino == DFS_INVALID_INODE) {
        dfs_inodeUnlock(sdir);
        if (tdir) {
            dfs_inodeUnlock(tdir);
        }
        dfs_reportError(__func__, __LINE__, parent, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    target = dfs_dirLookup(fs, tdir ? tdir : sdir, newname);

    /* Renaming to another directory */
    if (parent != newparent) {
        if (target != DFS_INVALID_INODE) {
            dremove(fs, tdir, newname, target, false);
        }
        inode = dfs_getInode(fs, ino, NULL, true, true);
        if (inode == NULL) {
            dfs_inodeUnlock(sdir);
            dfs_inodeUnlock(tdir);
            dfs_reportError(__func__, __LINE__, ino, ENOENT);
            fuse_reply_err(req, ENOENT);
            err = ENOENT;
            goto out;
        }
        dfs_dirAdd(tdir, ino, inode->i_stat.st_mode, newname);
        dfs_dirRemove(sdir, name);
        if (S_ISDIR(inode->i_stat.st_mode)) {
            assert(sdir->i_stat.st_nlink > 2);
            sdir->i_stat.st_nlink--;
            assert(tdir->i_stat.st_nlink >= 2);
            tdir->i_stat.st_nlink++;
        }
        inode->i_parent = dfs_getInodeHandle(newparent);
        dfs_inodeUnlock(inode);
    } else {

        /* Rename within the directory */
        if (target != DFS_INVALID_INODE) {
            dremove(fs, sdir, newname, target, false);
        }
        dfs_dirRename(sdir, ino, name, newname);
    }
    dfs_updateInodeTimes(sdir, false, true, true);
    if (tdir) {
        dfs_updateInodeTimes(tdir, false, true, true);
        dfs_inodeUnlock(tdir);
    }
    dfs_inodeUnlock(sdir);
    fuse_reply_err(req, 0);

out:
    dfs_statsAdd(fs, DFS_RENAME, err, &start);
    dfs_unlock(fs);
}

/* Create a new link to an inode */
static void
dfs_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
         const char *newname) {
    struct fuse_entry_param ep;
    struct inode *inode, *dir;
    struct timeval start;
    struct fs *fs;
    int err = 0;

    dfs_statsBegin(&start);
    dfs_displayEntry(__func__, newparent, ino, newname);
    fs = dfs_getfs(ino, false);
    if (fs->fs_snap) {
        dfs_reportError(__func__, __LINE__, ino, EROFS);
        fuse_reply_err(req, EROFS);
        err = EROFS;
        goto out;
    }
    dir = dfs_getInode(fs, newparent, NULL, true, true);
    if (dir == NULL) {
        dfs_reportError(__func__, __LINE__, newparent, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    assert(S_ISDIR(dir->i_stat.st_mode));
    inode = dfs_getInode(fs, ino, NULL, true, true);
    if (inode == NULL) {
        dfs_inodeUnlock(dir);
        dfs_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    assert(S_ISREG(inode->i_stat.st_mode));
    assert(dir->i_stat.st_nlink >= 2);
    dfs_dirAdd(dir, inode->i_stat.st_ino, inode->i_stat.st_mode, newname);
    dfs_updateInodeTimes(dir, false, true, true);
    inode->i_stat.st_nlink++;
    dfs_updateInodeTimes(inode, false, false, true);
    dfs_inodeUnlock(dir);
    memcpy(&ep.attr, &inode->i_stat, sizeof(struct stat));
    dfs_inodeUnlock(inode);
    ep.ino = dfs_setHandle(fs->fs_gindex, ino);
    dfs_epInit(&ep);
    fuse_reply_entry(req, &ep);

out:
    dfs_statsAdd(fs, DFS_LINK, err, &start);
    dfs_unlock(fs);
}

/* Set up file handle in case file is shared from another file system */
static int
dfs_openInode(struct fs *fs, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct inode *inode;
    bool modify;

    fi->fh = 0;
    modify = (fi->flags & (O_WRONLY | O_RDWR));
    if (modify && fs->fs_snap) {
        dfs_reportError(__func__, __LINE__, ino, EROFS);
        return EROFS;
    }
    inode = dfs_getInode(fs, ino, NULL, modify, true);
    if (inode == NULL) {
        dfs_reportError(__func__, __LINE__, ino, ENOENT);
        return ENOENT;
    }

    /* Do not allow opening a removed inode */
    if (inode->i_removed) {
        dfs_inodeUnlock(inode);
        dfs_reportError(__func__, __LINE__, ino, ENOENT);
        return ENOENT;
    }

    /* Increment open count if inode is private to this layer.
     */
    if (inode->i_fs == fs) {
        fi->keep_cache = inode->i_pcache;
        inode->i_ocount++;
    }
    fi->fh = (uint64_t)inode;
    dfs_inodeUnlock(inode);
    return 0;
}

/* Open a file and return a handle corresponding to the inode number */
static void
dfs_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct timeval start;
    struct fs *fs;
    int err;

    dfs_statsBegin(&start);
    dfs_displayEntry(__func__, 0, ino, NULL);
    fs = dfs_getfs(ino, false);
    err = dfs_openInode(fs, ino, fi);
    if (err) {
        fuse_reply_err(req, err);
    } else {
        fuse_reply_open(req, fi);
    }
    dfs_statsAdd(fs, DFS_OPEN, err, &start);
    dfs_unlock(fs);
}

/* Read from a file */
static void
dfs_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
         struct fuse_file_info *fi) {
    struct fuse_bufvec *bufv;
    struct timeval start;
    struct inode *inode;
    off_t endoffset;
    struct fs *fs;
    size_t fsize;
    int err = 0;

    dfs_statsBegin(&start);
    dfs_displayEntry(__func__, ino, 0, NULL);
    if (size == 0) {
        fuse_reply_buf(req, NULL, 0);
        return;
    }
    fsize = sizeof(struct fuse_bufvec) +
            (sizeof(struct fuse_buf) * ((size / DFS_BLOCK_SIZE) + 2));
    bufv = malloc(fsize);
    memset(bufv, 0, fsize);
    fs = dfs_getfs(ino, false);
    inode = dfs_getInode(fs, ino, (struct inode *)fi->fh, false, false);
    if (inode == NULL) {
        dfs_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    assert(S_ISREG(inode->i_stat.st_mode));

    /* Reading beyond file size is not allowed */
    fsize = inode->i_stat.st_size;
    if (off >= fsize) {
        dfs_inodeUnlock(inode);
        fuse_reply_buf(req, NULL, 0);
        goto out;
    }
    endoffset = off + size;
    if (endoffset > fsize) {
        endoffset = fsize;
    }
    dfs_readPages(inode, off, endoffset, bufv);
    fuse_reply_data(req, bufv, FUSE_BUF_SPLICE_MOVE);
    dfs_inodeUnlock(inode);

out:
    dfs_statsAdd(fs, DFS_READ, err, &start);
    dfs_unlock(fs);
    free(bufv);
}

/* Flush a file */
static void
dfs_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    /*
    struct timeval start;

    dfs_statsBegin(&start);
    */
    dfs_displayEntry(__func__, ino, 0, NULL);
    fuse_reply_err(req, 0);
    /*
    dfs_statsAdd(fs, DFS_FLUSH, err, &start);
    */
}

/* Decrement open count on an inode */
static void
dfs_releaseInode(struct fs *fs, fuse_ino_t ino,
                 struct fuse_file_info *fi, bool *inval) {
    struct inode *inode;

    assert(fi);
    inode = (struct inode *)fi->fh;
    if (inode->i_fs != fs) {
        if (inval) {
            *inval = true;
        }
        return;
    }
    dfs_inodeLock(inode, true);
    assert(inode->i_stat.st_ino == dfs_getInodeHandle(ino));
    assert(inode->i_fs == fs);
    assert(inode->i_ocount > 0);
    inode->i_ocount--;

    /* Truncate a removed file on last close */
    if ((inode->i_ocount == 0) && inode->i_removed &&
        S_ISREG(inode->i_stat.st_mode)) {
        dfs_truncate(inode, 0);
    }
    if (inval) {
        *inval = (inode->i_ocount == 0) && (inode->i_stat.st_size > 0) &&
                 (!inode->i_pcache || fs->fs_readOnly ||
                  (fs->fs_snap != NULL));
    }
    dfs_inodeUnlock(inode);
}

/* Release open count on a file */
static void
dfs_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct gfs *gfs = getfs();
    struct timeval start;
    struct fs *fs;
    bool inval;

    dfs_statsBegin(&start);
    dfs_displayEntry(__func__, ino, 0, NULL);
    fs = dfs_getfs(ino, false);
    dfs_releaseInode(fs, ino, fi, &inval);
    fuse_reply_err(req, 0);
    if (inval) {
        fuse_lowlevel_notify_inval_inode(gfs->gfs_ch, ino, 0, -1);
    }
    dfs_statsAdd(fs, DFS_RELEASE, false, &start);
    dfs_unlock(fs);
}

/* Sync a file */
static void
dfs_fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
          struct fuse_file_info *fi) {
    /*
    struct timeval start;

    dfs_statsBegin(&start);
    */
    dfs_displayEntry(__func__, ino, 0, NULL);
    fuse_reply_err(req, 0);
    /*
    dfs_statsAdd(fs, DFS_FSYNC, err, &start);
    */
}

/* Open a directory */
static void
dfs_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct timeval start;
    struct fs *fs;
    int err;

    dfs_statsBegin(&start);
    dfs_displayEntry(__func__, 0, ino, NULL);
    fs = dfs_getfs(ino, false);
    err = dfs_openInode(fs, ino, fi);
    if (err) {
        fuse_reply_err(req, err);
    } else {
        fuse_reply_open(req, fi);
    }
    dfs_statsAdd(fs, DFS_OPENDIR, err, &start);
    dfs_unlock(fs);
}

/* Read entries from a directory */
static void
dfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
            struct fuse_file_info *fi) {
    int count = 0, err = 0;
    struct dirent *dirent;
    struct timeval start;
    size_t esize, csize;
    struct inode *dir;
    char buf[size];
    struct stat st;
    struct fs *fs;

    dfs_statsBegin(&start);
    dfs_displayEntry(__func__, ino, 0, NULL);
    fs = dfs_getfs(ino, false);
    dir = dfs_getInode(fs, ino, (struct inode *)fi->fh, false, false);
    if (dir == NULL) {
        dfs_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    assert(S_ISDIR(dir->i_stat.st_mode));
    dirent = dir->i_dirent;
    while ((count < off) && dirent) {
        dirent = dirent->di_next;
        count++;
    }
    memset(&st, 0, sizeof(struct stat));
    csize = 0;
    while (dirent != NULL) {
        assert(dirent->di_ino > DFS_ROOT_INODE);
        count++;
        st.st_ino = dfs_setHandle(dfs_getIndex(fs, ino, dirent->di_ino),
                                  dirent->di_ino);
        st.st_mode = dirent->di_mode;
        esize = fuse_add_direntry(req, &buf[csize], size - csize,
                                  dirent->di_name, &st, count);
        csize += esize;
        if (csize >= size) {
            csize -= esize;
            break;
        }
        dirent = dirent->di_next;
    }
    dfs_inodeUnlock(dir);
    if (csize) {
        fuse_reply_buf(req, buf, csize);
    } else {
        fuse_reply_buf(req, NULL, 0);
    }

out:
    dfs_statsAdd(fs, DFS_READDIR, err, &start);
    dfs_unlock(fs);
}

/* Release a directory */
static void
dfs_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
    struct timeval start;
    struct fs *fs;

    dfs_statsBegin(&start);
    dfs_displayEntry(__func__, ino, 0, NULL);
    fs = dfs_getfs(ino, false);
    dfs_releaseInode(fs, ino, fi, NULL);
    fuse_reply_err(req, 0);
    dfs_statsAdd(fs, DFS_RELEASEDIR, false, &start);
    dfs_unlock(fs);
}

static void
dfs_fsyncdir(fuse_req_t req, fuse_ino_t ino, int datasync,
             struct fuse_file_info *fi) {
    /*
    struct timeval start;

    dfs_statsBegin(&start);
    */
    dfs_displayEntry(__func__, ino, 0, NULL);
    fuse_reply_err(req, 0);
    /*
    dfs_statsAdd(fs, DFS_FSYNCDIR, err, &start);
    */
}

/* File system statfs */
static void
dfs_statfs(fuse_req_t req, fuse_ino_t ino) {
    struct gfs *gfs = getfs();
    struct super *super = gfs->gfs_super;
    struct timeval start;
    struct statvfs buf;

    dfs_statsBegin(&start);
    dfs_displayEntry(__func__, ino, 0, NULL);
    memset(&buf, 0, sizeof(struct statvfs));
    buf.f_bsize = DFS_BLOCK_SIZE;
    buf.f_frsize = DFS_BLOCK_SIZE;
    buf.f_blocks = super->sb_tblocks;
    buf.f_bfree = buf.f_blocks - super->sb_blocks;
    buf.f_bavail = buf.f_bfree;
    buf.f_files = UINT32_MAX;
    buf.f_ffree = buf.f_files - super->sb_inodes;
    buf.f_favail = buf.f_ffree;
    buf.f_namemax = DFS_FILENAME_MAX;
    fuse_reply_statfs(req, &buf);
    dfs_statsAdd(dfs_getGlobalFs(gfs), DFS_STATFS, false, &start);
}

/* Set extended attributes on a file, currently used for creating a new file
 * system
 */
static void
dfs_setxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
             const char *value, size_t size, int flags) {
    dfs_displayEntry(__func__, ino, 0, name);
    dfs_xattrAdd(req, ino, name, value, size, flags);
}

/* Get extended attributes of the specified inode */
static void
dfs_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name, size_t size) {
    struct gfs *gfs = getfs();

    dfs_displayEntry(__func__, ino, 0, name);
    if (!gfs->gfs_xattr_enabled) {
        fuse_reply_err(req, ENODATA);
        return;
    }

    /* XXX Figure out a way to avoid invoking this for system.posix_acl_access
     * and system.posix_acl_default.
     */
    dfs_xattrGet(req, ino, name, size);
}

/* List extended attributes on a file */
static void
dfs_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size) {
    dfs_displayEntry(__func__, ino, 0, NULL);
    dfs_xattrList(req, ino, size);
}

/* Remove extended attributes */
static void
dfs_removexattr(fuse_req_t req, fuse_ino_t ino, const char *name) {
    dfs_displayEntry(__func__, ino, 0, name);
    dfs_xattrRemove(req, ino, name);
}

#if 0
/* Check access permissions on an inode */
static void
dfs_access(fuse_req_t req, fuse_ino_t ino, int mask) {
    dfs_displayEntry(__func__, 0, ino, NULL);
    fuse_reply_err(req, 0);
}
#endif

/* Create a file */
static void
dfs_create(fuse_req_t req, fuse_ino_t parent, const char *name,
           mode_t mode, struct fuse_file_info *fi) {
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    struct fuse_entry_param e;
    struct timeval start;
    struct fs *fs;
    int err;

    dfs_statsBegin(&start);
    dfs_displayEntry(__func__, parent, 0, name);
    fs = dfs_getfs(parent, false);
    err = create(fs, parent, name, S_IFREG | (mode & ~ctx->umask),
                 ctx->uid, ctx->gid, 0, NULL, fi, &e);
    if (err) {
        fuse_reply_err(req, err);
    } else {
        fuse_reply_create(req, &e, fi);
    }
    dfs_statsAdd(fs, DFS_CREATE, err, &start);
    dfs_unlock(fs);
}

#if 0
static void
dfs_getlk(fuse_req_t req, fuse_ino_t ino,
          struct fuse_file_info *fi, struct flock *lock) {
    dfs_displayEntry(__func__, ino);
}

static void
dfs_setlk(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi,
          struct flock *lock, int sleep) {
    dfs_displayEntry(__func__, ino);
}

static void
dfs_bmap(fuse_req_t req, fuse_ino_t ino, size_t blocksize, uint64_t idx) {
    dfs_displayEntry(__func__, ino);
}
#endif

static void
dfs_ioctl(fuse_req_t req, fuse_ino_t ino, int cmd, void *arg,
          struct fuse_file_info *fi, unsigned flags,
          const void *in_buf, size_t in_bufsz, size_t out_bufsz) {
    char name[in_bufsz + 1], *snap, *parent;
    struct gfs *gfs = getfs();
    int len, op, err = ENOSYS;

    dfs_displayEntry(__func__, ino, cmd, NULL);
    if (ino != gfs->gfs_snap_root) {
        //dfs_reportError(__func__, __LINE__, ino, ENOSYS);
        fuse_reply_err(req, ENOSYS);
        return;
    }
    if (in_bufsz > 0) {
        memcpy(name, in_buf, in_bufsz);
    }
    name[in_bufsz] = 0;
    op = _IOC_NR(cmd);
    switch (op) {
    case SNAP_CREATE:
    case CLONE_CREATE:
        len = _IOC_TYPE(cmd);
        if (len) {
            parent = name;
            name[len] = 0;
            snap = &name[len + 1];
        } else {
            parent = "";
            len = 0;
            snap = name;
        }
        dfs_newClone(req, gfs, snap, parent, len, op == CLONE_CREATE);
        return;

    case SNAP_REMOVE:
        dfs_removeClone(req, gfs, ino, name);
        return;

    case SNAP_MOUNT:
    case SNAP_STAT:
    case SNAP_UMOUNT:
    case UMOUNT_ALL:
        err = dfs_snap(gfs, name, op);
        break;
    }
    if (err) {
        fuse_reply_err(req, err);
    } else {
        fuse_reply_ioctl(req, 0, NULL, 0);
    }
}

#if 0
static void
dfs_poll(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi,
         struct fuse_pollhandle *ph) {
    dfs_displayEntry(__func__, ino);
}
#endif

/* Write provided data to file at the specified offset */
static void
dfs_write_buf(fuse_req_t req, fuse_ino_t ino,
              struct fuse_bufvec *bufv, off_t off, struct fuse_file_info *fi) {
    struct fuse_bufvec *dst;
    struct timeval start;
    struct inode *inode;
    size_t size, wsize;
    int count, err = 0;
    off_t endoffset;
    struct fs *fs;

    dfs_statsBegin(&start);
    dfs_displayEntry(__func__, ino, 0, NULL);
    size = bufv->buf[bufv->idx].size;
    wsize = sizeof(struct fuse_bufvec) +
           (sizeof(struct fuse_buf) * ((size / DFS_BLOCK_SIZE) + 2));
    dst = malloc(wsize);
    memset(dst, 0, wsize);
    endoffset = off + size;
    fs = dfs_getfs(ino, false);
    if (fs->fs_snap) {
        dfs_reportError(__func__, __LINE__, ino, EROFS);
        fuse_reply_err(req, EROFS);
        err = EROFS;
        goto out;
    }
    inode = dfs_getInode(fs, ino, (struct inode *)fi->fh, true, true);
    if (inode == NULL) {
        dfs_reportError(__func__, __LINE__, ino, ENOENT);
        fuse_reply_err(req, ENOENT);
        err = ENOENT;
        goto out;
    }
    assert(S_ISREG(inode->i_stat.st_mode));

    /* Update inode size if needed */
    if (endoffset > inode->i_stat.st_size) {
        inode->i_stat.st_size = endoffset;
    }
    count = dfs_addPages(inode, off, size, bufv, dst);
    dfs_updateInodeTimes(inode, false, true, true);
    dfs_inodeUnlock(inode);
    if (count) {
        dfs_blockAlloc(fs, count);
    }
    fuse_reply_write(req, size);

out:
    dfs_statsAdd(fs, DFS_WRITE_BUF, err, &start);
    dfs_unlock(fs);
    free(dst);
}

#if 0
static void
dfs_retrieve_reply(fuse_req_t req, void *cookie, fuse_ino_t ino, off_t offset,
                   struct fuse_bufvec *bufv) {
    dfs_displayEntry(__func__, ino);
}

/* Forget multiple inodes */
static void
dfs_forget_multi(fuse_req_t req, size_t count,
                 struct fuse_forget_data *forgets) {
    dfs_displayEntry(__func__, 0, count, NULL);
}

static void
dfs_flock(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi, int op) {
    dfs_displayEntry(__func__, ino);
}

static void
dfs_fallocate(fuse_req_t req, fuse_ino_t ino, int mode,
              off_t offset, off_t length, struct fuse_file_info *fi) {
    dfs_displayEntry(__func__, ino);
}
#endif

/* Initialize a new file system */
static void
dfs_init(void *userdata, struct fuse_conn_info *conn) {
    printf("%s: capable 0x%x want 0x%x gfs %p\n", __func__,
           conn->capable, conn->want, userdata);
    conn->want |= FUSE_CAP_IOCTL_DIR;
}

/* Destroy a file system */
static void
dfs_destroy(void *fsp) {
    struct gfs *gfs = (struct gfs *)fsp;

    printf("%s: gfs %p\n", __func__, gfs);
    dfs_unmount(gfs);
}

/* Fuse operations registered with the fuse driver */
struct fuse_lowlevel_ops dfs_ll_oper = {
    .init       = dfs_init,
    .destroy    = dfs_destroy,
    .lookup     = dfs_lookup,
    //.forget     = dfs_forget,
	.getattr	= dfs_getattr,
    .setattr    = dfs_setattr,
	.readlink	= dfs_readlink,
	.mknod  	= dfs_mknod,
	.mkdir  	= dfs_mkdir,
	.unlink  	= dfs_unlink,
	.rmdir		= dfs_rmdir,
	.symlink	= dfs_symlink,
    .rename     = dfs_rename,
    .link       = dfs_link,
    .open       = dfs_open,
    .read       = dfs_read,
    .flush      = dfs_flush,
    .release    = dfs_release,
    .fsync      = dfs_fsync,
    .opendir    = dfs_opendir,
    .readdir    = dfs_readdir,
    .releasedir = dfs_releasedir,
    .fsyncdir   = dfs_fsyncdir,
    .statfs     = dfs_statfs,
    .setxattr   = dfs_setxattr,
    .getxattr   = dfs_getxattr,
    .listxattr  = dfs_listxattr,
    .removexattr  = dfs_removexattr,
    //.access     = dfs_access,
    .create     = dfs_create,
#if 0
    .getlk      = dfs_getlk,
    .setlk      = dfs_setlk,
    .bmap       = dfs_bmap,
#endif
    .ioctl      = dfs_ioctl,
#if 0
    .poll       = dfs_poll,
#endif
    .write_buf  = dfs_write_buf,
#if 0
    .retrieve_reply = dfs_retrieve_reply,
    .forget_multi = dfs_forget_multi,
    .flock      = dfs_flock,
    .fallocate  = dfs_fallocate,
#endif
};
