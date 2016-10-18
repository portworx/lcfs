#include "includes.h"

/* Create a new file system */
int
dfs_newClone(struct gfs *gfs, ino_t ino, const char *name) {
    bool base = strncmp(name, "/", 1) ? false : true;
    struct fs *fs, *pfs = NULL, *nfs, *rfs;
    struct inode *inode, *pdir;
    ino_t root, pinum;
    int err = 0;

    root = dfs_getInodeHandle(ino);

    /* Do not allow new file systems to be created on global root */
    if (root <= DFS_ROOT_INODE) {
        dfs_reportError(__func__, __LINE__, ino, EPERM);
        return EPERM;
    }
    rfs = dfs_getfs(DFS_ROOT_INODE, false);

    /* Do not allow nested file systems */
    if (dfs_getFsHandle(ino)) {
        err = EPERM;
        dfs_reportError(__func__, __LINE__, ino, err);
        goto out;
    }

    /* Get the new root directory inode */
    inode = dfs_getInode(rfs, ino, NULL, false, true);
    if (inode == NULL) {
        err = ENOENT;
        dfs_reportError(__func__, __LINE__, ino, err);
        goto out;
    }

    /* Do not allow creating file systems on non-directories or non-empty
     * directories.  Also snapshots can be created in /dfs directory only.
     */
    if (!S_ISDIR(inode->i_stat.st_mode) || (inode->i_dirent != NULL) ||
        (inode->i_parent != gfs->gfs_snap_root)) {
        err = EINVAL;
        dfs_reportError(__func__, __LINE__, ino, err);
        goto out;
    }
    dfs_inodeUnlock(inode);
    if (!base) {

        /* Lookup parent directory in global root file system */
        pdir = dfs_getInode(rfs, gfs->gfs_snap_root, NULL, true, true);
        if (pdir == NULL) {
            err = ENOENT;
            dfs_reportError(__func__, __LINE__, gfs->gfs_snap_root, err);
            goto out;
        }

        /* Find parent file system root */
        pinum = dfs_dirLookup(rfs, pdir, name);
        dfs_inodeUnlock(pdir);
        if (pinum == DFS_INVALID_INODE) {
            err = ENOENT;
            dfs_reportError(__func__, __LINE__, ino, err);
            goto out;
        }

        /* Get the parent file system root directory from the parent file
         * system.
         */
        pfs = dfs_getfs(pinum, true);
        assert(pfs->fs_root == pinum);
    }
    fs = dfs_newFs(gfs, root, base);
    if (base) {
        nfs = gfs->gfs_fs[0];
    } else {
        fs->fs_parent = pfs;
        fs->fs_ilock = pfs->fs_ilock;
        fs->fs_rwlock = pfs->fs_rwlock;
    }
    err = dfs_readInodes(fs);
    if (err != 0) {
        dfs_reportError(__func__, __LINE__, ino, err);
        goto out;
    }

    /* Check if this is a root file system or a snapshot/clone of another */
    if (base) {
        dfs_printf("Created new FS %p, no parent, root %ld\n",
                   fs, fs->fs_root);
    } else {

        /* Copy over root directory */
        pdir = dfs_getInode(pfs, pfs->fs_root, NULL, false, false);
        if (pdir == NULL) {
            err = ENOENT;
            dfs_reportError(__func__, __LINE__, pfs->fs_root, err);
            goto out;
        }

        /* Get the root directory of the new file system */
        inode = dfs_getInode(fs, root, NULL, false, true);
        if (inode == NULL) {
            dfs_inodeUnlock(pdir);
            err = ENOENT;
            dfs_reportError(__func__, __LINE__, ino, err);
            goto out;
        }
        dfs_dirCopy(inode, pdir);
        dfs_inodeUnlock(pdir);
        dfs_inodeUnlock(inode);
        dfs_printf("Created new FS %p, parent %ld root %ld\n",
                   fs, pfs->fs_root, fs->fs_root);

        /* Link this file system a snapshot of the parent */
        if (pfs->fs_snap != NULL) {
            nfs = pfs->fs_snap;
        } else {
            pfs->fs_snap = fs;
            nfs = NULL;
        }
    }

    /* Add this file system to global list of file systems */
    dfs_addfs(fs, nfs);

out:
    dfs_unlock(rfs);
    if (pfs) {
        dfs_unlock(pfs);
    }
    if (err && fs) {
        dfs_removeFs(fs);
    }
    return err;
}

/* Remove a file system */
int
dfs_removeClone(ino_t ino) {
    struct fs *fs;
    ino_t root;

    root = dfs_getInodeHandle(ino);
    if (root <= DFS_ROOT_INODE) {
        dfs_reportError(__func__, __LINE__, ino, EPERM);
        return EPERM;
    }

    /* There should be a file system rooted on this directory */
    fs = dfs_getfs(ino, true);
    if ((fs == NULL) || (fs->fs_root != root)) {
        dfs_unlock(fs);
        dfs_reportError(__func__, __LINE__, ino, ENOENT);
        return ENOENT;
    }
    dfs_printf("Removing file system with root inode %ld, fs %p\n", root, fs);

    /* Remove the file system from the global list */
    dfs_removefs(fs);
    dfs_unlock(fs);
    dfs_removeFs(fs);
    return 0;
}

