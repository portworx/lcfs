#include "includes.h"

/* Given a snapshot name, find its root inode */
ino_t
lc_getRootIno(struct fs *fs, ino_t parent, const char *name) {
    struct inode *dir;
    ino_t root;

    /* Lookup parent directory in global root file system */
    dir = lc_getInode(fs, parent, NULL, false, false);
    if (dir == NULL) {
        lc_reportError(__func__, __LINE__, parent, ENOENT);
        return LC_INVALID_INODE;
    }
    root = lc_dirLookup(fs, dir, name);
    lc_inodeUnlock(dir);
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
    struct fs *fs = NULL, *pfs = NULL, *nfs = NULL, *rfs = NULL;
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
    if (!base) {
        pinum = lc_getRootIno(rfs, gfs->gfs_snap_root, pname);
        if (pinum == LC_INVALID_INODE) {
            err = ENOENT;
            goto out;
        }
    }

    /* Create a new file system structure */
    fs = lc_newFs(gfs, rw);
    posix_memalign(&super, LC_BLOCK_SIZE, LC_BLOCK_SIZE);
    lc_superInit(super, 0, false);
    fs->fs_super = super;
    lc_lock(fs, true);

    /* Allocate root inode and add to the directory */
    root = lc_inodeAlloc(fs);
    fs->fs_root = root;
    fs->fs_super->sb_root = root;
    fs->fs_super->sb_flags |= LC_SUPER_DIRTY;
    if (rw) {
        fs->fs_super->sb_flags |= LC_SUPER_RDWR;
    }
    fs->fs_sblock = lc_blockAlloc(fs, 1, true);
    lc_rootInit(fs, fs->fs_root);
    if (err != 0) {
        lc_reportError(__func__, __LINE__, root, err);
        goto out;
    }
    if (base) {
        fs->fs_pcache = lc_pcache_init();
        fs->fs_ilock = malloc(sizeof(pthread_mutex_t));
        pthread_mutex_init(fs->fs_ilock, NULL);
        nfs = lc_getGlobalFs(gfs);
    } else {

        /* Get the root directory of the new file system */
        dir = lc_getInode(fs, root, NULL, false, true);
        if (dir == NULL) {
            err = ENOENT;
            lc_reportError(__func__, __LINE__, root, err);
            goto out;
        }

        /* Inode chain lock is shared with the parent */
        pfs = lc_getfs(pinum, true);
        assert(pfs->fs_root == lc_getInodeHandle(pinum));

        /* Copy the root directory of the parent file system */
        pdir = lc_getInode(pfs, pfs->fs_root, NULL, false, false);
        if (pdir == NULL) {
            lc_inodeUnlock(dir);
            err = ENOENT;
            lc_reportError(__func__, __LINE__, pfs->fs_root, err);
            lc_unlock(pfs);
            goto out;
        }

        dir->i_stat.st_nlink = pdir->i_stat.st_nlink;
        dir->i_dirent = pdir->i_dirent;
        dir->i_shared = true;
        lc_dirCopy(dir);
        lc_inodeUnlock(pdir);
        lc_inodeUnlock(dir);

        /* Link this file system a snapshot of the parent */
        nfs = pfs->fs_snap;
        if (nfs == NULL) {
            pfs->fs_snap = fs;
            pfs->fs_super->sb_childSnap = fs->fs_sblock;
            pfs->fs_super->sb_flags |= LC_SUPER_DIRTY;
        }
        fs->fs_parent = pfs;
        fs->fs_pcache = pfs->fs_pcache;
        fs->fs_ilock = pfs->fs_ilock;
    }

    /* Add this file system to global list of file systems */
    lc_addfs(fs, nfs);
    if (pfs) {
        lc_unlock(pfs);
    }

    /* Add the new directory to the parent directory */
    pdir = lc_getInode(rfs, gfs->gfs_snap_root, NULL, false, true);
    if (pdir == NULL) {
        err = ENOENT;
        lc_reportError(__func__, __LINE__, gfs->gfs_snap_root, err);
        goto out;
    }
    lc_dirAdd(pdir, root, S_IFDIR, name, strlen(name));
    pdir->i_stat.st_nlink++;
    lc_markInodeDirty(pdir, true, true, false, false);
    lc_updateInodeTimes(pdir, false, true, true);
    lc_inodeUnlock(pdir);

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
            lc_destroyFs(fs, true);
        }
    }
}

/* Remove a layer */
void
lc_removeClone(fuse_req_t req, struct gfs *gfs, ino_t ino, const char *name) {
    struct fs *fs = NULL, *rfs;
    struct timeval start;
    struct inode *pdir;
    int err = 0;
    ino_t root;

    /* Find the inode in snapshot directory */
    lc_statsBegin(&start);
    assert(ino == gfs->gfs_snap_root);
    rfs = lc_getfs(LC_ROOT_INODE, false);
    root = lc_getRootIno(rfs, ino, name);
    if (root == LC_INVALID_INODE) {
        err = ENOENT;
        goto out;
    }

    /* There should be a file system rooted on this directory */
    fs = lc_getfs(root, true);
    if (fs == NULL) {
        lc_reportError(__func__, __LINE__, root, ENOENT);
        err = ENOENT;
        goto out;
    }
    if (fs->fs_root != lc_getInodeHandle(root)) {
        lc_unlock(fs);
        lc_reportError(__func__, __LINE__, root, EINVAL);
        err = EINVAL;
        goto out;
    }
    if (fs->fs_snap) {
        lc_unlock(fs);
        lc_reportError(__func__, __LINE__, root, EEXIST);
        err = EEXIST;
        goto out;
    }
    lc_printf("Removing fs with parent %ld root %ld index %d name %s\n",
               fs->fs_parent ? fs->fs_parent->fs_root : - 1,
               fs->fs_root, fs->fs_gindex, name);
    fs->fs_removed = true;
    lc_invalidateDirtyPages(gfs, fs);

    /* Remove the file system from the snapshot chain */
    lc_removeSnap(gfs, fs);

    /* Remove the root directory */
    pdir = lc_getInode(rfs, ino, NULL, false, true);
    if (pdir == NULL) {
        err = ENOENT;
        lc_reportError(__func__, __LINE__, ino, err);
        goto out;
    }
    lc_dirRemoveInode(pdir, fs->fs_root);
    assert(pdir->i_stat.st_nlink > 2);
    pdir->i_stat.st_nlink--;
    lc_markInodeDirty(pdir, true, true, false, false);
    lc_updateInodeTimes(pdir, false, true, true);
    lc_inodeUnlock(pdir);

out:
    if (err) {
        fuse_reply_err(req, err);
    } else {
        fuse_reply_ioctl(req, 0, NULL, 0);
    }
    lc_statsAdd(rfs, LC_CLONE_REMOVE, err, &start);
    lc_unlock(rfs);
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
}

/* Mount, unmount, stat a snapshot */
int
lc_snap(struct gfs *gfs, const char *name, enum ioctl_cmd cmd) {
    struct timeval start;
    struct fs *fs, *rfs;
    ino_t root;
    int err;

    lc_statsBegin(&start);
    rfs = lc_getGlobalFs(gfs);

    /* Unmount all layers */
    if (cmd == UMOUNT_ALL) {
        /* XXX Do this in the background */
        lc_umountAll(gfs);
        lc_statsAdd(rfs, LC_CLEANUP, 0, &start);
        return 0;
    }
    root = lc_getRootIno(rfs, gfs->gfs_snap_root, name);
    err = (root == LC_INVALID_INODE) ? ENOENT : 0;
    switch (cmd) {
    case SNAP_MOUNT:
        if (err == 0) {
            fs = lc_getfs(root, true);
            fs->fs_super->sb_flags |= LC_SUPER_DIRTY;
            lc_unlock(fs);
        }
        lc_statsAdd(rfs, LC_MOUNT, err, &start);
        break;

    case SNAP_STAT:
    case SNAP_UMOUNT:
        if (err == 0) {
            fs = lc_getfs(root, false);
            lc_displayStats(fs);
            lc_unlock(fs);
        }
        lc_statsAdd(rfs, cmd == SNAP_UMOUNT ? LC_UMOUNT : LC_STAT,
                     err, &start);
        break;

    case CLEAR_STAT:
        if (err == 0) {
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
    return err;
}

