#include "includes.h"

/* Given a snapshot name, find its root inode */
ino_t
dfs_getRootIno(struct fs *fs, ino_t parent, const char *name) {
    struct inode *dir;
    ino_t root;

    /* Lookup parent directory in global root file system */
    dir = dfs_getInode(fs, parent, NULL, false, false);
    if (dir == NULL) {
        dfs_reportError(__func__, __LINE__, parent, ENOENT);
        return DFS_INVALID_INODE;
    }
    root = dfs_dirLookup(fs, dir, name);
    dfs_inodeUnlock(dir);
    if (root == DFS_INVALID_INODE) {
        dfs_reportError(__func__, __LINE__, parent, ENOENT);
    } else {
        root = dfs_setHandle(dfs_getIndex(fs, parent, root), root);
    }
    return root;
}

/* Create a new file system */
void
dfs_newClone(fuse_req_t req, struct gfs *gfs, const char *name,
             const char *parent, size_t size, bool rw) {
    struct fs *fs = NULL, *pfs = NULL, *nfs = NULL, *rfs = NULL;
    struct inode *dir, *pdir;
    ino_t root, pinum = 0;
    struct timeval start;
    char pname[size + 1];
    void *super;
    int err = 0;
    bool base;

    dfs_statsBegin(&start);

    /* Check if parent is specified */
    if (size) {
        memcpy(pname, parent, size);
        pname[size] = 0;
        base = false;
    } else {
        base = true;
    }
    rfs = dfs_getfs(DFS_ROOT_INODE, false);
    if (!base) {
        pinum = dfs_getRootIno(rfs, gfs->gfs_snap_root, pname);
        if (pinum == DFS_INVALID_INODE) {
            err = ENOENT;
            goto out;
        }
    }

    /* Create a new file system structure */
    fs = dfs_newFs(gfs, rw);
    posix_memalign(&super, DFS_BLOCK_SIZE, DFS_BLOCK_SIZE);
    dfs_superInit(super, 0, false);
    fs->fs_super = super;
    dfs_lock(fs, true);

    /* Allocate root inode and add to the directory */
    root = dfs_inodeAlloc(fs);
    fs->fs_root = root;
    fs->fs_super->sb_root = root;
    fs->fs_super->sb_flags |= DFS_SUPER_DIRTY;
    if (rw) {
        fs->fs_super->sb_flags |= DFS_SUPER_RDWR;
    }
    fs->fs_sblock = dfs_blockAlloc(fs, 1);
    dfs_rootInit(fs, fs->fs_root);
    if (err != 0) {
        dfs_reportError(__func__, __LINE__, root, err);
        goto out;
    }
    if (base) {
        fs->fs_pcache = dfs_pcache_init();
        fs->fs_ilock = malloc(sizeof(pthread_mutex_t));
        pthread_mutex_init(fs->fs_ilock, NULL);
        nfs = dfs_getGlobalFs(gfs);
    } else {

        /* Get the root directory of the new file system */
        dir = dfs_getInode(fs, root, NULL, false, true);
        if (dir == NULL) {
            err = ENOENT;
            dfs_reportError(__func__, __LINE__, root, err);
            goto out;
        }

        /* Inode chain lock is shared with the parent */
        pfs = dfs_getfs(pinum, true);
        assert(pfs->fs_root == dfs_getInodeHandle(pinum));

        /* Copy the root directory of the parent file system */
        pdir = dfs_getInode(pfs, pfs->fs_root, NULL, false, false);
        if (pdir == NULL) {
            dfs_inodeUnlock(dir);
            err = ENOENT;
            dfs_reportError(__func__, __LINE__, pfs->fs_root, err);
            dfs_unlock(pfs);
            goto out;
        }

        dir->i_stat.st_nlink = pdir->i_stat.st_nlink;
        dir->i_dirent = pdir->i_dirent;
        dir->i_shared = true;
        dfs_dirCopy(dir);
        dfs_inodeUnlock(pdir);
        dfs_inodeUnlock(dir);

        /* Link this file system a snapshot of the parent */
        nfs = pfs->fs_snap;
        if (nfs == NULL) {
            pfs->fs_snap = fs;
            pfs->fs_super->sb_childSnap = fs->fs_sblock;
            pfs->fs_super->sb_flags |= DFS_SUPER_DIRTY;
        }
        fs->fs_parent = pfs;
        fs->fs_pcache = pfs->fs_pcache;
        fs->fs_ilock = pfs->fs_ilock;
    }

    /* Add this file system to global list of file systems */
    dfs_addfs(fs, nfs);
    if (pfs) {
        dfs_unlock(pfs);
    }

    /* Add the new directory to the parent directory */
    pdir = dfs_getInode(rfs, gfs->gfs_snap_root, NULL, false, true);
    if (pdir == NULL) {
        err = ENOENT;
        dfs_reportError(__func__, __LINE__, gfs->gfs_snap_root, err);
        goto out;
    }
    dfs_dirAdd(pdir, root, S_IFDIR, name, strlen(name));
    pdir->i_stat.st_nlink++;
    dfs_markInodeDirty(pdir, true, true, false, false);
    dfs_updateInodeTimes(pdir, false, true, true);
    dfs_inodeUnlock(pdir);

    dfs_printf("Created fs with parent %ld root %ld index %d block %ld name %s\n",
               pfs ? pfs->fs_root : -1, root, fs->fs_gindex, fs->fs_sblock, name);

out:
    if (err) {
        fuse_reply_err(req, err);
    } else {
        fuse_reply_ioctl(req, 0, NULL, 0);
    }
    dfs_statsAdd(rfs, DFS_CLONE_CREATE, err, &start);
    dfs_unlock(rfs);
    if (fs) {
        dfs_unlock(fs);
        if (err) {
            dfs_destroyFs(fs, true);
        }
    }
}

/* Remove a file system */
void
dfs_removeClone(fuse_req_t req, struct gfs *gfs, ino_t ino, const char *name) {
    struct fs *fs = NULL, *rfs;
    struct timeval start;
    struct inode *pdir;
    int err = 0;
    ino_t root;

    /* Find the inode in snapshot directory */
    dfs_statsBegin(&start);
    assert(ino == gfs->gfs_snap_root);
    rfs = dfs_getfs(DFS_ROOT_INODE, false);
    dfs_setupSpecialDir(gfs, rfs);
    root = dfs_getRootIno(rfs, ino, name);
    if (root == DFS_INVALID_INODE) {
        err = ENOENT;
        goto out;
    }

    /* There should be a file system rooted on this directory */
    fs = dfs_getfs(root, true);
    if (fs == NULL) {
        dfs_reportError(__func__, __LINE__, root, ENOENT);
        err = ENOENT;
        goto out;
    }
    if (fs->fs_root != dfs_getInodeHandle(root)) {
        dfs_unlock(fs);
        dfs_reportError(__func__, __LINE__, root, EINVAL);
        err = EINVAL;
        goto out;
    }
    if (fs->fs_snap) {
        dfs_unlock(fs);
        dfs_reportError(__func__, __LINE__, root, EEXIST);
        err = EEXIST;
        goto out;
    }
    dfs_printf("Removing fs with parent %ld root %ld index %d name %s\n",
               fs->fs_parent ? fs->fs_parent->fs_root : - 1,
               fs->fs_root, fs->fs_gindex, name);

    /* Remove the file system from the snapshot chain */
    dfs_removeSnap(gfs, fs);

    /* Remove the root directory */
    pdir = dfs_getInode(rfs, ino, NULL, false, true);
    if (pdir == NULL) {
        err = ENOENT;
        dfs_reportError(__func__, __LINE__, ino, err);
        goto out;
    }
    dfs_dirRemoveInode(pdir, fs->fs_root);
    assert(pdir->i_stat.st_nlink > 2);
    pdir->i_stat.st_nlink--;
    dfs_markInodeDirty(pdir, true, true, false, false);
    dfs_updateInodeTimes(pdir, false, true, true);
    dfs_inodeUnlock(pdir);

out:
    if (err) {
        fuse_reply_err(req, err);
    } else {
        fuse_reply_ioctl(req, 0, NULL, 0);
    }
    dfs_statsAdd(rfs, DFS_CLONE_REMOVE, err, &start);
    dfs_unlock(rfs);
    if (fs) {

        /* Remove the file system from the global list and notify VFS layer */
        if (!err) {
            fuse_lowlevel_notify_delete(gfs->gfs_ch, ino, root, name,
                                        strlen(name));
            dfs_removefs(gfs, fs);
        }
        dfs_unlock(fs);
        if (!err) {
            dfs_destroyFs(fs, true);
        }
    }
}

/* Mount, unmount, stat a snapshot */
int
dfs_snap(struct gfs *gfs, const char *name, enum ioctl_cmd cmd) {
    struct timeval start;
    struct fs *fs, *rfs;
    ino_t root;
    int err;

    dfs_statsBegin(&start);
    rfs = dfs_getGlobalFs(gfs);
    if (cmd == UMOUNT_ALL) {
        dfs_umountAll(gfs);
        dfs_statsAdd(rfs, DFS_CLEANUP, 0, &start);
        return 0;
    }
    root = dfs_getRootIno(rfs, gfs->gfs_snap_root, name);
    err = (root == DFS_INVALID_INODE) ? ENOENT : 0;
    switch (cmd) {
    case SNAP_MOUNT:
        dfs_statsAdd(rfs, DFS_MOUNT, err, &start);
        break;

    case SNAP_STAT:
        dfs_statsAdd(rfs, DFS_STAT, err, &start);
        break;

    case SNAP_UMOUNT:
        if (err == 0) {
            fs = dfs_getfs(root, true);
            dfs_syncInodes(gfs, fs);
            dfs_displayStats(fs);
            dfs_unlock(fs);
        }
        dfs_statsAdd(rfs, DFS_UMOUNT, err, &start);
        break;

    default:
        err = EINVAL;
    }
    return err;
}

