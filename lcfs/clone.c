#include "includes.h"

/* Given a snapshot name, find its root inode */
ino_t
lc_getRootIno(struct fs *fs, const char *name, struct inode *pdir) {
    ino_t parent = fs->fs_gfs->gfs_snap_root;
    struct inode *dir = pdir ? pdir : fs->fs_gfs->gfs_snap_rootInode;
    ino_t root;

    /* Lookup parent directory in global root file system */
    if (pdir == NULL) {
        lc_inodeLock(dir, false);
    }
    root = lc_dirLookup(fs, dir, name);
    if (pdir == NULL) {
        lc_inodeUnlock(dir);
    }
    if (root == LC_INVALID_INODE) {
        lc_reportError(__func__, __LINE__, parent, ENOENT);
    } else {
        root = lc_setHandle(lc_getIndex(fs, parent, root), root);
    }
    return root;
}

/* Create a new layer */
void
lc_newClone(fuse_req_t req, struct gfs *gfs, const char *name,
             const char *parent, size_t size, bool rw) {
    struct fs *fs = NULL, *pfs = NULL, *rfs = NULL;
    struct inode *dir, *pdir;
    ino_t root, pinum = 0;
    struct timeval start;
    char pname[size + 1];
    void *super;
    int err = 0;
    bool base;

    lc_statsBegin(&start);

    /* Check if parent is specified */
    if (size) {
        memcpy(pname, parent, size);
        pname[size] = 0;
        base = false;
    } else {
        base = true;
    }

    /* Get the global file system */
    rfs = lc_getfs(LC_ROOT_INODE, false);
    if (!lc_hasSpace(gfs, LC_LAYER_MIN_BLOCKS)) {
        err = ENOSPC;
        goto out;
    }
    root = lc_inodeAlloc(rfs);
    pdir = gfs->gfs_snap_rootInode;
    lc_inodeLock(pdir, true);
    if (!base) {
        pinum = lc_getRootIno(rfs, pname, pdir);
        if (pinum == LC_INVALID_INODE) {
            lc_inodeUnlock(pdir);
            err = ENOENT;
            goto out;
        }
    }

    /* Allocate root inode and add to the directory */
    lc_dirAdd(pdir, root, S_IFDIR, name, strlen(name));
    pdir->i_stat.st_nlink++;
    lc_markInodeDirty(pdir, true, true, false, false);
    lc_updateInodeTimes(pdir, false, true, true);
    lc_inodeUnlock(pdir);

    /* Initialize the new layer */
    fs = lc_newFs(gfs, rw);
    lc_lock(fs, true);
    malloc_aligned((void **)&super);
    lc_superInit(super, 0, false);
    fs->fs_super = super;
    fs->fs_root = root;
    fs->fs_super->sb_root = root;
    fs->fs_super->sb_flags |= LC_SUPER_DIRTY | LC_SUPER_MOUNTED;
    if (rw) {
        fs->fs_super->sb_flags |= LC_SUPER_RDWR;
    }
    lc_rootInit(fs, fs->fs_root);
    if (base) {
        fs->fs_pcache = lc_pcache_init();
        fs->fs_ilock = malloc(sizeof(pthread_mutex_t));
        pthread_mutex_init(fs->fs_ilock, NULL);
    } else {
        dir = fs->fs_rootInode;
        dir->i_shared = true;

        /* Copy the parent root directory */
        pfs = lc_getfs(pinum, false);
        assert(pfs->fs_root == lc_getInodeHandle(pinum));
        pdir = pfs->fs_rootInode;
        lc_inodeLock(pdir, false);
        dir->i_stat.st_nlink = pdir->i_stat.st_nlink;
        dir->i_dirent = pdir->i_dirent;
        lc_dirCopy(dir);
        lc_inodeUnlock(pdir);

        /* Inode chain lock is shared with the parent */
        fs->fs_parent = pfs;
        fs->fs_pcache = pfs->fs_pcache;
        fs->fs_ilock = pfs->fs_ilock;
    }

    /* Add this file system to global list of file systems */
    lc_addfs(fs, pfs);
    if (pfs) {
        lc_unlock(pfs);
    }
    lc_printf("Created fs with parent %ld root %ld index %d block %ld name %s\n",
               pfs ? pfs->fs_root : -1, root, fs->fs_gindex, fs->fs_sblock, name);

out:
    if (err) {
        fuse_reply_err(req, err);
    } else {
        fuse_reply_ioctl(req, 0, NULL, 0);
    }
    lc_statsAdd(rfs, LC_CLONE_CREATE, err, &start);
    lc_unlock(rfs);
    if (fs) {
        lc_unlock(fs);
        if (err) {
            fs->fs_removed = true;
            lc_destroyFs(fs, true);
        }
    }
}

/* Check if a layer could be removed */
static int
lc_removeLayer(struct fs *rfs, struct inode *dir, ino_t ino, bool rmdir,
               void **fsp) {
    struct fs *fs;
    ino_t root;

    /* There should be a file system rooted on this directory */
    root = lc_setHandle(lc_getIndex(rfs, dir->i_stat.st_ino, ino), ino);
    if (lc_getFsHandle(root) == 0) {
        lc_reportError(__func__, __LINE__, root, ENOENT);
        return ENOENT;
    }
    fs = lc_getfs(root, false);
    if (fs == NULL) {
        lc_reportError(__func__, __LINE__, root, ENOENT);
        return ENOENT;
    }
    if (fs->fs_root != ino) {
        lc_unlock(fs);
        lc_reportError(__func__, __LINE__, root, EINVAL);
        return EINVAL;
    }
    if (fs->fs_snap) {
        lc_unlock(fs);
        lc_reportError(__func__, __LINE__, root, EEXIST);
        return EEXIST;
    }
    fs->fs_removed = true;
    lc_unlock(fs);
    fs = lc_getfs(root, true);
    assert(fs->fs_root == ino);
    assert(fs->fs_removed);
    *fsp = fs;
    return 0;
}

/* Remove a layer */
void
lc_removeClone(fuse_req_t req, struct gfs *gfs, const char *name) {
    ino_t ino = gfs->gfs_snap_root;
    struct fs *fs = NULL, *rfs;
    struct inode *pdir = NULL;
    struct timeval start;
    int err = 0;
    ino_t root;

    /* Find the inode in snapshot directory */
    lc_statsBegin(&start);
    rfs = lc_getfs(LC_ROOT_INODE, false);
    pdir = gfs->gfs_snap_rootInode;
    lc_inodeLock(pdir, true);
    err = lc_dirRemoveName(rfs, pdir, name, true,
                           (void **)&fs, lc_removeLayer);
    if (err) {
        lc_inodeUnlock(pdir);
        fuse_reply_err(req, err);
        goto out;
    }

    /* Remove the file system from the snapshot chain */
    fs->fs_super->sb_flags &= ~(LC_SUPER_DIRTY | LC_SUPER_MOUNTED);
    lc_removeSnap(gfs, fs);
    lc_inodeUnlock(pdir);
    fuse_reply_ioctl(req, 0, NULL, 0);
    root = fs->fs_root;

    lc_printf("Removing fs with parent %ld root %ld index %d name %s\n",
               fs->fs_parent ? fs->fs_parent->fs_root : - 1,
               fs->fs_root, fs->fs_gindex, name);
    lc_invalidateDirtyPages(gfs, fs);
    lc_invalidateInodePages(gfs, fs);
    lc_invalidateInodeBlocks(gfs, fs);
    lc_blockFree(fs, fs->fs_sblock, 1);

out:
    if (fs) {

        /* Remove the file system from the global list and notify VFS layer */
        if (!err) {
            fuse_lowlevel_notify_delete(gfs->gfs_ch, ino, root, name,
                                        strlen(name));
            lc_freeLayerBlocks(gfs, fs, true);
            lc_removefs(gfs, fs);
        }
        lc_unlock(fs);
        if (!err) {
            lc_destroyFs(fs, true);
        }
    }
    lc_statsAdd(rfs, LC_CLONE_REMOVE, err, &start);
    lc_unlock(rfs);
}

/* Mount, unmount, stat a snapshot */
void
lc_snapIoctl(fuse_req_t req, struct gfs *gfs, const char *name,
             enum ioctl_cmd cmd) {
    struct timeval start;
    struct fs *fs, *rfs;
    ino_t root;
    int err;

    lc_statsBegin(&start);
    rfs = lc_getfs(LC_ROOT_INODE, false);

    /* Unmount all layers */
    if (cmd == UMOUNT_ALL) {
        fuse_reply_ioctl(req, 0, NULL, 0);
        lc_umountAll(gfs);
        lc_statsAdd(rfs, LC_CLEANUP, 0, &start);
        lc_unlock(rfs);
        return;
    }
    root = lc_getRootIno(rfs, name, NULL);
    err = (root == LC_INVALID_INODE) ? ENOENT : 0;
    switch (cmd) {
    case SNAP_MOUNT:
        if (err == 0) {
            fuse_reply_ioctl(req, 0, NULL, 0);
            fs = lc_getfs(root, true);
            fs->fs_super->sb_flags |= LC_SUPER_DIRTY | LC_SUPER_MOUNTED;
            lc_unlock(fs);
        }
        lc_statsAdd(rfs, LC_MOUNT, err, &start);
        break;

    case SNAP_STAT:
    case SNAP_UMOUNT:
        if (err == 0) {
            fuse_reply_ioctl(req, 0, NULL, 0);
            fs = lc_getfs(root, false);
            lc_displayStats(fs);
            lc_unlock(fs);
            if ((cmd == SNAP_UMOUNT) && fs->fs_readOnly) {
                lc_sync(gfs, fs);
            }
        }
        lc_statsAdd(rfs, cmd == SNAP_UMOUNT ? LC_UMOUNT : LC_STAT,
                     err, &start);
        break;

    case CLEAR_STAT:
        if (err == 0) {
            fuse_reply_ioctl(req, 0, NULL, 0);
            fs = lc_getfs(root, true);
            lc_displayStats(fs);
            lc_statsDeinit(fs);
            fs->fs_stats = lc_statsNew();
            lc_unlock(fs);
        }
        break;

    default:
        err = EINVAL;
    }
    if (err) {
        lc_reportError(__func__, __LINE__, root, err);
        fuse_reply_err(req, err);
    }
    lc_unlock(rfs);
}

