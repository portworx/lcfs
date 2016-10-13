#include "includes.h"

/* Create a new file system */
int
dfs_newClone(struct gfs *gfs, ino_t ino, const char *name) {
    struct fs *fs, *pfs, *nfs, *rfs;
    struct inode *inode, *pdir;
    ino_t root, pinum;
    int err = 0;

    root = dfs_getInodeHandle(ino);

    /* Do not allow new file systems to be created on global root */
    if (root <= DFS_ROOT_INODE) {
        dfs_reportError(__func__, ino, EPERM);
        return EPERM;
    }
    dfs_lock(gfs, true);

    /* Do not allow nested file systems */
    if (dfs_getFsHandle(ino) != DFS_ROOT_INODE) {
        err = EPERM;
        dfs_reportError(__func__, ino, err);
        goto out;
    }

    /* Get the new root directory inode */
    rfs = dfs_getfs(gfs, DFS_ROOT_INODE);
    inode = dfs_getInode(rfs, ino, NULL, false, true);
    if (inode == NULL) {
        err = ENOENT;
        dfs_reportError(__func__, ino, err);
        goto out;
    }

    /* Do not allow creating file systems on non-directories or non-empty
     * directories.  Also snapshots can be created in /dfs directory only.
     */
    if (!S_ISDIR(inode->i_stat.st_mode) || (inode->i_dirent != NULL) ||
        (inode->i_parent != gfs->gfs_snap_root)) {
        err = EINVAL;
        dfs_reportError(__func__, ino, err);
        goto out;
    }
    dfs_inodeUnlock(inode);
    fs = malloc(sizeof(struct fs));
    memset(fs, 0, sizeof(struct fs));
    fs->fs_root = root;
    fs->fs_gfs = gfs;
    err = dfs_readInodes(fs);
    if (err != 0) {
        dfs_reportError(__func__, ino, err);
        goto out;
    }

    /* Check if this is a root file system or a snapshot/clone of another */
    if (strcmp(name, "/") == 0) {
        nfs = gfs->gfs_fs;
        dfs_printf("Created new FS %p, no parent, root %ld\n",
                   fs, fs->fs_root);
    } else {

        /* Lookup parent directory in global root file system */
        pdir = dfs_getInode(rfs, gfs->gfs_snap_root, NULL, true, true);
        if (pdir == NULL) {
            err = ENOENT;
            dfs_reportError(__func__, gfs->gfs_snap_root, err);
            goto out;
        }

        /* Find parent file system root */
        pinum = dfs_dirLookup(rfs, pdir, name);
        dfs_inodeUnlock(pdir);
        if (pinum == DFS_INVALID_INODE) {
            err = ENOENT;
            dfs_reportError(__func__, ino, err);
            goto out;
        }

        /* Get the parent file system root directory from the parent file
         * system.
         */
        pfs = dfs_getfs(gfs, pinum);
        pdir = dfs_getInode(pfs, pinum, NULL, false, false);
        if (pdir == NULL) {
            err = ENOENT;
            dfs_reportError(__func__, pinum, err);
            goto out;
        }

        /* Get the root directory of the new file system */
        inode = dfs_getInode(fs, root, NULL, false, true);
        if (inode == NULL) {
            dfs_inodeUnlock(pdir);
            err = ENOENT;
            dfs_reportError(__func__, ino, err);
            goto out;
        }

        /* Copy over root directory */
        dfs_dirCopy(inode, pdir);
        dfs_inodeUnlock(pdir);
        dfs_inodeUnlock(inode);
        fs->fs_parent = pfs;

        /* Link this file system a snapshot of the parent */
        if (pfs->fs_snap != NULL) {
            nfs = pfs->fs_snap;
        } else {
            pfs->fs_snap = fs;
            nfs = NULL;
        }
        dfs_printf("Created new FS %p, parent %ld root %ld\n", fs, pfs->fs_root, fs->fs_root);
    }

    /* Add this file system to the snapshot list or root file systems list */
    if (nfs != NULL) {
        fs->fs_next = nfs->fs_next;
        nfs->fs_next = fs;
    }

    /* Add this file system to global list of file systems */
    pfs = gfs->gfs_fs;
    while (pfs) {
        if (pfs->fs_gnext == NULL) {
            pfs->fs_gnext = fs;
            break;
        }
        pfs = pfs->fs_gnext;
    }

out:
    dfs_unlock(gfs);
    return err;
}

/* Remove a file system */
int
dfs_removeClone(struct gfs *gfs, ino_t ino) {
    struct fs *fs, *pfs, *nfs;
    ino_t root;

    root = dfs_getInodeHandle(ino);
    if (root <= DFS_ROOT_INODE) {
        dfs_reportError(__func__, ino, EPERM);
        return EPERM;
    }
    dfs_lock(gfs, true);

    /* There should be a file system rooted on this directory */
    fs = dfs_getfs(gfs, ino);
    if ((fs == NULL) || (fs->fs_root != root)) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, ino, ENOENT);
        return ENOENT;
    }
    dfs_printf("Removing file system with root inode %ld, fs %p\n", root, fs);
    assert(fs->fs_snap == NULL);
    pfs = gfs->gfs_fs;

    /* Remove the file system from the global list */
    if (pfs == fs) {
        gfs->gfs_fs = fs->fs_gnext;
    } else {
        while (pfs) {
            if (pfs->fs_gnext == fs) {
                pfs->fs_gnext = fs->fs_gnext;
                break;
            }
            pfs = pfs->fs_gnext;
        }
    }

    /* Remove the file system from the snapshot list */
    pfs = fs->fs_parent;
    if (pfs && (pfs->fs_snap == fs)) {
        pfs->fs_snap = fs->fs_next;
    } else {
        if (pfs) {
            nfs = pfs->fs_snap;
        } else {
            nfs = gfs->gfs_fs;
        }
        while (nfs) {
            if (nfs->fs_next == fs) {
                nfs->fs_next = fs->fs_next;
                break;
            }
            nfs = nfs->fs_next;
        }
    }
    dfs_unlock(gfs);
    return 0;
}

