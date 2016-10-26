#include "includes.h"

/* Create a new file system */
void
dfs_newClone(fuse_req_t req, struct gfs *gfs, const char *name,
             const char *parent, size_t size, bool rw) {
    struct fs *fs = NULL, *pfs = NULL, *nfs = NULL, *rfs = NULL;
    struct inode *dir, *pdir;
    ino_t root, pinum = 0;
    char pname[size + 1];
    int err = 0;
    bool base;

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
        /* Lookup parent directory in global root file system */
        pdir = dfs_getInode(rfs, gfs->gfs_snap_root, NULL, false, false);
        if (pdir == NULL) {
            err = ENOENT;
            dfs_reportError(__func__, __LINE__, gfs->gfs_snap_root, err);
            goto out;
        }
        pinum = dfs_dirLookup(rfs, pdir, pname);
        dfs_inodeUnlock(pdir);
        if (pinum == DFS_INVALID_INODE) {
            err = ENOENT;
            dfs_reportError(__func__, __LINE__, gfs->gfs_snap_root, err);
            goto out;
        }
        pinum = dfs_setHandle(dfs_getIndex(rfs, gfs->gfs_snap_root, pinum),
                              pinum);
    }

    /* Create a new file system structure */
    fs = dfs_newFs(gfs, rw, true);
    dfs_lock(fs, true);

    /* Allocate root inode and add to the directory */
    root = dfs_inodeAlloc(fs);
    fs->fs_root = root;
    err = dfs_readInodes(fs);
    if (err != 0) {
        dfs_reportError(__func__, __LINE__, root, err);
        goto out;
    }
    if (base) {
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

        dfs_dirCopy(dir, pdir);
        dfs_inodeUnlock(pdir);
        dfs_inodeUnlock(dir);

        /* Link this file system a snapshot of the parent */
        nfs = pfs->fs_snap;
        if (nfs == NULL) {
            pfs->fs_snap = fs;
        }
        fs->fs_parent = pfs;
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
    dfs_dirAdd(pdir, root, S_IFDIR, name);
    pdir->i_stat.st_nlink++;
    dfs_updateInodeTimes(pdir, false, true, true);
    dfs_inodeUnlock(pdir);

    dfs_printf("Created FS %p, parent %ld root %ld index %d name %s\n",
               fs, pfs ? pfs->fs_root : -1, root, fs->fs_gindex, name);

out:
    dfs_unlock(rfs);
    if (fs) {
        dfs_unlock(fs);
        if (err) {
            dfs_destroyFs(fs);
        }
    }
    fuse_reply_err(req, err);

    /* Invalidate page cache of the previous layer when snapshot is taken on it
     * first time.
     */
    if ((err == 0) && (nfs == NULL)) {
        pfs = dfs_getfs(pinum, false);
        assert(pfs->fs_root == dfs_getInodeHandle(pinum));
        dfs_invalidate_pcache(gfs, pfs);
        dfs_unlock(pfs);
    }
}

/* Remove a file system */
void
dfs_removeClone(fuse_req_t req, struct gfs *gfs, ino_t ino, const char *name) {
    struct fs *fs = NULL, *rfs;
    struct inode *pdir;
    int err = 0;
    ino_t root;

    /* Find the inode in snapshot directory */
    assert(ino == gfs->gfs_snap_root);
    rfs = dfs_getfs(DFS_ROOT_INODE, false);
    dfs_setupSpecialDir(gfs, rfs);
    pdir = dfs_getInode(rfs, ino, NULL, false, false);
    if (pdir == NULL) {
        err = ENOENT;
        dfs_reportError(__func__, __LINE__, ino, err);
        goto out;
    }
    root = dfs_dirLookup(rfs, pdir, name);
    dfs_inodeUnlock(pdir);
    if (root == DFS_INVALID_INODE) {
        dfs_reportError(__func__, __LINE__, root, ENOENT);
        err = ENOENT;
        goto out;
    }
    root = dfs_setHandle(dfs_getIndex(rfs, ino, root), root);

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
    dfs_printf("Removing FS %p, parent %ld root %ld index %d name %s\n",
               fs, fs->fs_parent ? fs->fs_parent->fs_root : - 1,
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
    dfs_updateInodeTimes(pdir, false, true, true);
    dfs_inodeUnlock(pdir);

out:
    dfs_unlock(rfs);
    fuse_reply_err(req, err);
    if (fs) {

        /* Remove the file system from the global list and notify VFS layer */
        if (!err) {
            fuse_lowlevel_notify_delete(gfs->gfs_ch, ino, root, name,
                                        strlen(name));
            dfs_removefs(gfs, fs);
        }
        dfs_unlock(fs);
        if (!err) {
            dfs_destroyFs(fs);
        }
    }
}

