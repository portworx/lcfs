#include "includes.h"

/* Create a new file system */
int
dfs_newClone(ino_t ino, ino_t pino, const char *name) {
    struct inode *inode, *pdir;
    struct fs *fs, *pfs, *nfs;
    struct gfs *gfs = getfs();
    ino_t pinode;
    int err = 0;

    dfs_lock(gfs, true);
    inode = dfs_getInode(dfs_getfs(gfs, DFS_ROOT_INODE), ino, false, true);
    if (inode == NULL) {
        err = -ENOENT;
        goto out;
    }

    /* Do not allow creating file systems on non-directories or non-empty
     * directories.
     */
    if (((inode->i_stat.st_mode & S_IFMT) != S_IFDIR) ||
        (inode->i_dirent != NULL)) {
        err = -EINVAL;
        goto out;
    }
    dfs_inodeUnlock(inode);
    fs = malloc(sizeof(struct fs));
    memset(fs, 0, sizeof(struct fs));
    fs->fs_root = ino;
    fs->fs_gfs = gfs;
    err = dfs_readInodes(fs);
    if (err != 0) {
        goto out;
    }

    /* Check if this is a root file system or a snapshot/clone of another */
    if (strcmp(name, "/") == 0) {
        nfs = gfs->gfs_fs;
        dfs_printf("Created new FS %p, no parent, root %ld\n",
                   fs, fs->fs_root);
    } else {

        /* Lookup parent file system */
        pinode = dfs_dirLookup(dfs_getfs(gfs, DFS_ROOT_INODE), pino, name);
        if (pinode == DFS_INVALID_INODE) {
            dfs_unlock(gfs);
            return -ENOENT;
        }
        pfs = dfs_getfs(gfs, pinode);
        pdir = dfs_getInode(pfs, pinode, false, false);
        if (pdir == NULL) {
            dfs_unlock(gfs);
            return -ENOENT;
        }
        inode = dfs_getInode(fs, ino, false, true);
        if (inode == NULL) {
            dfs_inodeUnlock(pdir);
            dfs_unlock(gfs);
            return -ENOENT;
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
dfs_removeClone(const char *path) {
    struct gfs *gfs = getfs();
    struct fs *fs, *pfs, *nfs;
    struct inode *inode;
    int err = 0;
    ino_t ino;

    dfs_lock(gfs, true);
    inode = dfs_getPathInode(path, gfs, false, true);
    if (inode == NULL) {
        dfs_unlock(gfs);
        return -ENOENT;
    }
    ino = inode->i_stat.st_ino;
    fs = dfs_getfs(gfs, ino);
    dfs_printf("Removing file system with root inode %ld, fs %p\n", ino, fs);
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
    dfs_inodeUnlock(inode);
    dfs_unlock(gfs);
    return err;
}

