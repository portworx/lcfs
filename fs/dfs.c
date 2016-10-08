#define FUSE_USE_VERSION 29
#define _GNU_SOURCE

#include "includes.h"

#define ARGC    4

/* Create a new directory entry and associated inode */
static uint64_t
create(const char *path, mode_t mode, dev_t rdev, const char *target) {
    struct fuse_context *fc = fuse_get_context();
    struct gfs *gfs = fc->private_data;
    char name[DFS_FILENAME_MAX];
    struct inode *dir;
    ino_t ino, parent;
    struct fs *fs;

    dfs_lock(gfs, false);
    dfs_lookup(path, gfs, &fs, &parent, name);
    if (parent == DFS_INVALID_INODE) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, path, 0, ENOENT);
        return -ENOENT;
    }
    dir = dfs_getInode(fs, parent, true, true);
    if (dir == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, path, parent, ENOENT);
        return -ENOENT;
    }
    assert((dir->i_stat.st_mode & S_IFMT) == S_IFDIR);
    ino = dfs_inodeInit(fs, mode, fc->uid, fc->gid, rdev, target);
    dfs_dirAdd(dir, ino, mode, name);
    if ((mode & S_IFMT) == S_IFDIR) {
        dir->i_stat.st_nlink++;
    }
    dfs_updateInodeTimes(dir, false, true, true);
    dfs_inodeUnlock(dir);
    dfs_unlock(gfs);
    return dfs_setHandle(fs, ino);
}

/* Remove a directory entry */
static int
dremove(struct fs *fs, struct inode *dir, char *name, ino_t ino, bool rmdir) {
    struct inode * inode = dfs_getInode(fs, ino, true, true);

    if (inode == NULL) {
        dfs_reportError(__func__, NULL, ino, ESTALE);
        return -ESTALE;
    }
    assert(inode->i_stat.st_nlink);
    if (rmdir) {
        if (inode->i_stat.st_nlink > 2) {
        /*
        if ((inode->i_stat.st_nlink > 2) ||
            (inode->i_dirent != NULL)) {
        */
            dfs_inodeUnlock(inode);
            dfs_reportError(__func__, NULL, ino, EEXIST);
            return -EEXIST;
        }
        dir->i_stat.st_nlink--;
        inode->i_removed = true;
    } else {
        inode->i_stat.st_nlink--;

        /* Flag a file as removed on last unlink */
        if (inode->i_stat.st_nlink == 0) {
            inode->i_removed = true;
        }
    }
    dfs_dirRemove(dir, name);
    dfs_updateInodeTimes(dir, false, false, true);
    dfs_inodeUnlock(inode);
    return 0;
}

/* Remove a directory entry */
static int
dfs_remove(const char *path, bool rmdir) {
    struct gfs *gfs = getfs();
    char name[DFS_FILENAME_MAX];
    struct inode *dir;
    ino_t ino, parent;
    struct fs *fs;
    int err;

    dfs_lock(gfs, false);
    ino = dfs_lookup(path, gfs, &fs, &parent, name);
    if ((ino == DFS_INVALID_INODE) ||
        (parent == DFS_INVALID_INODE)) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, path, 0, ESTALE);
        return -ESTALE;
    }
    dir = dfs_getInode(fs, parent, true, true);
    if (dir == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, path, parent, ENOENT);
        return -ENOENT;
    }
    assert((dir->i_stat.st_mode & S_IFMT) == S_IFDIR);
    err = dremove(fs, dir, name, ino, rmdir);
    dfs_inodeUnlock(dir);
    dfs_unlock(gfs);
    return err;
}

/* Get attributes of a file */
static int
dfs_getattr(const char *path, struct stat *stat) {
    struct gfs *gfs = getfs();
    struct inode *inode;

    dfs_displayEntry(__func__, path, 0);
    dfs_lock(gfs, false);
    inode = dfs_getPathInode(path, gfs, false, false);
    if (inode == NULL) {
        dfs_unlock(gfs);
        //dfs_reportError(__func__, path, 0, ENOENT);
        return -ENOENT;
    }
    memcpy(stat, &inode->i_stat, sizeof(struct stat));
    dfs_inodeUnlock(inode);
    dfs_unlock(gfs);
    return 0;
}

/* Read target information for a symbolic link */
static int
dfs_readlink(const char *path, char *buf, size_t len) {
    struct gfs *gfs = getfs();
    struct inode *inode;
    int size;

    dfs_displayEntry(__func__, path, 0);
    dfs_lock(gfs, false);
    inode = dfs_getPathInode(path, gfs, false, false);
    if (inode == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, path, 0, ENOENT);
        return -ENOENT;
    }
    assert((inode->i_stat.st_mode & S_IFMT) == S_IFLNK);
    size = strlen(inode->i_target);
    if (size > len) {
        size = len;
    }
    strncpy(buf, inode->i_target, size);
    dfs_inodeUnlock(inode);
    buf[size] = 0;
    dfs_unlock(gfs);
    return 0;
}

#if 0
static int
dfs_getdir(const char *path, fuse_dirh_t h, fuse_dirfil_t df) {
    dfs_displayEntry(__func__, path, 0);
    return 0;
}
#endif

/* Create a directory */
static int
dfs_mkdir(const char *path, mode_t mode) {
    dfs_displayEntry(__func__, path, 0);
    create(path, S_IFDIR | mode, 0, NULL);
    return 0;
}

/* Create a special file */
static int
dfs_mknod(const char *path, mode_t mode, dev_t rdev) {
    dfs_displayEntry(__func__, path, 0);
    create(path, mode, rdev, NULL);
    return 0;
}

/* Remove a file */
static int
dfs_unlink(const char *path) {
    dfs_displayEntry(__func__, path, 0);
    return dfs_remove(path, false);
}

/* Remove a special directory */
static int
dfs_rmdir(const char *path) {
    dfs_displayEntry(__func__, path, 0);
    return dfs_remove(path, true);
}

/* Create a symbolic link */
static int
dfs_symlink(const char *path, const char *linkname) {
    dfs_displayEntry(__func__, path, 0);
    create(linkname, S_IFLNK | 0777, 0, path);
    return 0;
}

/* Rename a file to another (mv) */
static int
dfs_rename(const char *oldpath, const char *newpath) {
    char name[DFS_FILENAME_MAX], oldname[DFS_FILENAME_MAX];
    struct inode *inode, *sdir, *tdir;
    ino_t ino, target, dest, source;
    struct gfs *gfs = getfs();
    struct fs *fs, *nfs;

    dfs_displayEntry(__func__, oldpath, 0);
    dfs_lock(gfs, false);
    ino = dfs_lookup(oldpath, gfs, &fs, &source, oldname);
    target = dfs_lookup(newpath, gfs, &nfs, &dest, name);
    assert(fs == nfs);

    /* Follow some locking order while locking the directories */
    if (source > dest) {
        tdir = dfs_getInode(fs, dest, true, true);
        if (tdir == NULL) {
            dfs_unlock(gfs);
            dfs_reportError(__func__, newpath, dest, ENOENT);
            return -ENOENT;
        }
    }
    sdir = dfs_getInode(fs, source, true, true);
    if (sdir == NULL) {
        if (tdir) {
            dfs_inodeUnlock(tdir);
        }
        dfs_unlock(gfs);
        dfs_reportError(__func__, oldpath, source, ENOENT);
        return -ENOENT;
    }
    assert((sdir->i_stat.st_mode & S_IFMT) == S_IFDIR);
    if (source < dest) {
        tdir = dfs_getInode(fs, dest, true, true);
        if (tdir == NULL) {
            dfs_inodeUnlock(sdir);
            dfs_unlock(gfs);
            dfs_reportError(__func__, newpath, dest, ENOENT);
            return -ENOENT;
        }
        assert((tdir->i_stat.st_mode & S_IFMT) == S_IFDIR);
    }

    /* Renaming to another directory */
    if (source != dest) {
        if (target != DFS_INVALID_INODE) {
            dremove(fs, tdir, name, target, false);
        }
        inode = dfs_getInode(fs, ino, true, true);
        if (inode == NULL) {
            dfs_inodeUnlock(sdir);
            dfs_inodeUnlock(tdir);
            dfs_unlock(gfs);
            dfs_reportError(__func__, oldpath, ino, ENOENT);
            return -ENOENT;
        }
        dfs_dirAdd(tdir, ino, inode->i_stat.st_mode, name);
        dfs_dirRemove(sdir, oldname);
        if ((inode->i_stat.st_mode & S_IFMT) == S_IFDIR) {
            assert(sdir->i_stat.st_nlink);
            sdir->i_stat.st_nlink--;
            tdir->i_stat.st_nlink++;
        }
        dfs_inodeUnlock(inode);
    } else {

        /* Rename within the directory */
        if (target != DFS_INVALID_INODE) {
            dremove(fs, sdir, name, target, false);
        }
        dfs_dirRename(sdir, ino, name);
        tdir = NULL;
    }
    dfs_updateInodeTimes(sdir, false, true, true);
    if (tdir) {
        dfs_updateInodeTimes(tdir, false, true, true);
        dfs_inodeUnlock(tdir);
    }
    dfs_inodeUnlock(sdir);
    dfs_unlock(gfs);
    return 0;
}

/* Create a new link to an inode */
static int
dfs_link(const char *oldpath, const char *newpath) {
    char name[DFS_FILENAME_MAX];
    struct gfs *gfs = getfs();
    struct inode *inode, *dir;
    struct fs *fs, *nfs;
    ino_t ino, parent;

    dfs_displayEntry(__func__, oldpath, 0);
    dfs_lock(gfs, false);
    ino = dfs_lookup(oldpath, gfs, &fs, NULL, NULL);
    if (ino == DFS_INVALID_INODE) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, oldpath, 0, ENOENT);
        return -ENOENT;
    }
    dfs_lookup(newpath, gfs, &nfs, &parent, name);
    assert(fs == nfs);
    if (parent == DFS_INVALID_INODE) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, newpath, 0, ENOENT);
        return -ENOENT;
    }
    dir = dfs_getInode(fs, parent, true, true);
    if (dir == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, newpath, parent, ENOENT);
        return -ENOENT;
    }
    assert((dir->i_stat.st_mode & S_IFMT) == S_IFDIR);
    inode = dfs_getInode(fs, ino, true, true);
    if (inode == NULL) {
        dfs_inodeUnlock(dir);
        dfs_unlock(gfs);
        dfs_reportError(__func__, oldpath, ino, ENOENT);
        return -ENOENT;
    }
    assert((inode->i_stat.st_mode & S_IFMT) == S_IFREG);
    dfs_dirAdd(dir, ino, inode->i_stat.st_mode, name);
    dfs_updateInodeTimes(dir, false, true, true);
    inode->i_stat.st_nlink++;
    dfs_updateInodeTimes(inode, false, false, true);
    dfs_inodeUnlock(inode);
    dfs_inodeUnlock(dir);
    dfs_unlock(gfs);
    return 0;
}

/* Change permissions on a file */
static int
dfs_chmod(const char *path, mode_t mode) {
    struct gfs *gfs = getfs();
    struct inode *inode;

    dfs_displayEntry(__func__, path, 0);
    dfs_lock(gfs, false);
    inode = dfs_getPathInode(path, gfs, true, true);

    if (inode == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, path, 0, ENOENT);
        return -ENOENT;
    }
    assert((inode->i_stat.st_mode & S_IFMT) == (mode & S_IFMT));
    inode->i_stat.st_mode = mode;
    dfs_updateInodeTimes(inode, false, false, true);
    dfs_inodeUnlock(inode);
    dfs_unlock(gfs);
    return 0;
}

/* Change owner/group on a file */
static int
dfs_chown(const char *path, uid_t uid, gid_t gid) {
    struct gfs *gfs = getfs();
    struct inode *inode;

    dfs_displayEntry(__func__, path, 0);
    dfs_lock(gfs, false);
    inode = dfs_getPathInode(path, gfs, true, true);

    if (inode == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, path, 0, ENOENT);
        return -ENOENT;
    }
    if (uid != -1) {
        inode->i_stat.st_uid = uid;
    }
    if (gid != -1) {
        inode->i_stat.st_gid = gid;
    }
    dfs_updateInodeTimes(inode, false, false, true);
    dfs_inodeUnlock(inode);
    dfs_unlock(gfs);
    return 0;
}

/* Truncate a file */
static void
dtruncate(struct inode *inode, off_t size) {
    assert((inode->i_stat.st_mode & S_IFMT) == S_IFREG);
    if (size < inode->i_stat.st_size) {
        dfs_truncPages(inode, size);
    }
    inode->i_stat.st_size = size;
    dfs_updateInodeTimes(inode, false, false, true);
    dfs_inodeUnlock(inode);
}

/* Truncate the file specified by the path */
static int
dfs_truncate(const char *path, off_t size) {
    struct gfs *gfs = getfs();
    struct inode *inode;

    dfs_displayEntry(__func__, path, 0);
    dfs_lock(gfs, false);
    inode = dfs_getPathInode(path, gfs, true, true);
    if (inode == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, path, 0, ENOENT);
        return -ENOENT;
    }
    dtruncate(inode, size);
    dfs_unlock(gfs);
    return 0;
}

#if 0
static int
dfs_utime(const char *path, struct utimbuf *tv) {
    dfs_displayEntry(__func__, path, 0);
    return 0;
}
#endif

/* Open a file and return a handle corresponding to the path */
static int
dfs_open(const char *path, struct fuse_file_info *fi) {
    struct gfs *gfs = getfs();
    struct fs *fs;
    ino_t ino;

    dfs_displayEntry(__func__, path, 0);
    dfs_lock(gfs, false);
    ino = dfs_lookup(path, gfs, &fs, NULL, NULL);
    if (ino == DFS_INVALID_INODE) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, path, 0, ENOENT);
        return -ENOENT;
    }
    fi->fh = dfs_setHandle(fs, ino);
    dfs_unlock(gfs);
    return 0;
}

/* Read from a file */
static int
dfs_read(const char *path, char *buf, size_t size, off_t off,
         struct fuse_file_info *fi) {
    struct gfs *gfs = getfs();
    struct inode *inode;
    off_t endoffset;
    size_t fsize;

    dfs_displayEntry(__func__, path, fi->fh);
    if (size == 0) {
        return 0;
    }
    dfs_lock(gfs, false);
    inode = dfs_getInode(dfs_getfs(gfs, dfs_getFsHandle(fi->fh)),
                         dfs_getInodeHandle(fi->fh), false, false);
    if (inode == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, path, fi->fh, ENOENT);
        return -ENOENT;
    }
    assert((inode->i_stat.st_mode & S_IFMT) == S_IFREG);

    /* Reading beyond file size is not allowed */
    fsize = inode->i_stat.st_size;
    if (off >= fsize) {
        dfs_inodeUnlock(inode);
        dfs_unlock(gfs);
        return 0;
    }
    endoffset = off + size;
    if (endoffset > fsize) {
        endoffset = fsize;
    }
    dfs_readPages(inode, off, endoffset, buf);
    dfs_inodeUnlock(inode);
    dfs_unlock(gfs);
    return endoffset - off;
}

/* Write to a file */
static int
dfs_write(const char *path, const char *buf, size_t size, off_t off,
          struct fuse_file_info *fi) {
    off_t endoffset, poffset, woff = 0;
    struct gfs *gfs = getfs();
    uint64_t page, spage;
    struct inode *inode;
    size_t wsize, psize;

    dfs_displayEntry(__func__, path, fi->fh);
    spage = off / DFS_BLOCK_SIZE;
    endoffset = off + size;
    page = spage;
    wsize = size;
    dfs_lock(gfs, false);
    inode = dfs_getInode(dfs_getfs(gfs, dfs_getFsHandle(fi->fh)),
                         dfs_getInodeHandle(fi->fh), true, true);
    if (inode == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, path, fi->fh, ENOENT);
        return -ENOENT;
    }
    assert((inode->i_stat.st_mode & S_IFMT) == S_IFREG);

    /* Break the down the write into pages and link those to the file */
    while (wsize) {
        if (page == spage) {
            poffset = off % DFS_BLOCK_SIZE;
            psize = DFS_BLOCK_SIZE - poffset;
        } else {
            poffset = 0;
            psize = DFS_BLOCK_SIZE;
        }
        if (psize > wsize) {
            psize = wsize;
        }
        dfs_addPage(inode, page, poffset, psize, &buf[woff]);
        page++;
        woff += psize;
        wsize -= psize;
    }

    /* Update inode size if needed */
    if (endoffset > inode->i_stat.st_size) {
        inode->i_stat.st_size = endoffset;
    }
    dfs_updateInodeTimes(inode, false, true, true);

    /* Update block count */
    dfs_inodeUnlock(inode);
    dfs_unlock(gfs);
    return size;
}

/* File system statfs */
static int
dfs_statfs(const char *path, struct statvfs *buf) {
    struct gfs *gfs = getfs();

    dfs_displayEntry(__func__, path, 0);
    buf->f_bsize = DFS_BLOCK_SIZE;
    buf->f_frsize = DFS_BLOCK_SIZE;
    buf->f_blocks = gfs->gfs_super->sb_tblocks;
    buf->f_bfree = buf->f_blocks - gfs->gfs_super->sb_nblock;
    buf->f_bavail = buf->f_bfree;
    buf->f_files = UINT32_MAX;
    buf->f_ffree = buf->f_files - gfs->gfs_super->sb_ninode;
    buf->f_favail = buf->f_ffree;
    buf->f_fsid = 0;
    buf->f_flag = 0;
    buf->f_namemax = DFS_FILENAME_MAX;
    return 0;
}

/* Flush a file */
static int
dfs_flush(const char *path, struct fuse_file_info *fi) {
    dfs_displayEntry(__func__, path, fi->fh);
    return 0;
}

/* Release open count on a file */
static int
dfs_release(const char *path, struct fuse_file_info *fi) {
    dfs_displayEntry(__func__, path, fi->fh);
    return 0;
}

/* Sync a file */
static int
dfs_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
    dfs_displayEntry(__func__, path, fi->fh);
    return 0;
}

/* Set extended attributes on a file, currently used for creating a new file
 * system
 */
static int
dfs_setxattr(const char *path, const char *name, const char *value,
             size_t size, int flags) {
    struct gfs *gfs = getfs();
    ino_t ino, pino;

    dfs_displayEntry(__func__, path, 0);
    if (strstr(path, "/dfs/") != path) {
        dfs_reportError(__func__, path, 0, EPERM);
        return -EPERM;
    }
    dfs_lock(gfs, false);
    ino = dfs_lookup(path, gfs, NULL, &pino, NULL);
    dfs_unlock(gfs);
    if ((ino == DFS_INVALID_INODE) ||
        (pino == DFS_INVALID_INODE)) {
        dfs_reportError(__func__, path, 0, ENOENT);
        return -ENOENT;
    }
    dfs_printf("Creating a clone %s, parent %s\n", path, name);
    return dfs_newClone(ino, pino, name);
}

#if 0
static int
dfs_getxattr(const char *path, const char *name, char *value, size_t size) {
    dfs_displayEntry(__func__, path, 0);
    return 0;
}
#endif

/* List extended attributes on a file */
static int
dfs_listxattr(const char *path, char *list, size_t size) {
    dfs_displayEntry(__func__, path, 0);
    if (strstr(path, "/dfs/") != path) {
        dfs_reportError(__func__, path, 0, EPERM);
        return -EPERM;
    }
    return 0;
}

/* Remove extended attributes */
static int
dfs_removexattr(const char *path, const char *name) {
    dfs_displayEntry(__func__, path, 0);
    if (strstr(path, "/dfs/") != path) {
        dfs_reportError(__func__, path, 0, EPERM);
        return -EPERM;
    }
    dfs_printf("Removing a clone %s\n", path);
    return dfs_removeClone(path);
}

/* Open a directory */
static int
dfs_opendir(const char *path, struct fuse_file_info *fi) {
    struct gfs *gfs = getfs();
    struct fs *fs;
    ino_t ino;

    dfs_displayEntry(__func__, path, 0);
    dfs_lock(gfs, false);
    ino = dfs_lookup(path, gfs, &fs, NULL, NULL);
    if (ino == DFS_INVALID_INODE) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, path, 0, ENOENT);
        return -ENOENT;
    }
    fi->fh = dfs_setHandle(fs, ino);
    dfs_unlock(gfs);
    return 0;
}

/* Read entries from a directory */
static int
dfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t off,
            struct fuse_file_info *fi) {
    struct gfs *gfs = getfs();
    struct dirent *dirent;
    struct inode *dir;
    struct stat st;
    int count = 0;
    struct fs *fs;

    dfs_displayEntry(__func__, path, fi->fh);
    dfs_lock(gfs, false);
    fs = dfs_getfs(gfs, dfs_getFsHandle(fi->fh));
    dir = dfs_getInode(fs, dfs_getInodeHandle(fi->fh), false, false);
    if (dir == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, path, fi->fh, ENOENT);
        return -ENOENT;
    }
    assert((dir->i_stat.st_mode & S_IFMT) == S_IFDIR);
    dirent = dir->i_dirent;
    while ((count < off) && dirent) {
        dirent = dirent->di_next;
        count++;
    }
    memset(&st, 0, sizeof(struct stat));
    while (dirent != NULL) {
        count++;
        st.st_ino = dirent->di_ino;
        st.st_mode = dirent->di_mode;
        if (filler(buf, dirent->di_name, &st, count)) {
            break;
        }
        dirent = dirent->di_next;
    }
    dfs_inodeUnlock(dir);
    dfs_unlock(gfs);
    return 0;
}

/* Release a directory */
static int
dfs_releasedir(const char *path, struct fuse_file_info *fi) {
    dfs_displayEntry(__func__, path, 0);
    return 0;
}

/*
static int
dfs_fsyncdir(const char *path, int datasync, struct fuse_file_info *fi) {
    dfs_displayEntry(__func__, path, 0);
    return 0;
}

static int
dfs_access(const char *path, int mask) {
    dfs_displayEntry(__func__, path, 0);
    return 0;
}
*/

/* Create a file */
static int
dfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    dfs_displayEntry(__func__, path, 0);
    fi->fh = create(path, S_IFREG | mode, 0, NULL);
    return 0;
}

/* Truncate a file using a file handle */
static int
dfs_ftruncate(const char *path, off_t off, struct fuse_file_info *fi) {
    struct gfs *gfs = getfs();
    struct inode *inode;

    dfs_displayEntry(__func__, path, fi->fh);
    dfs_lock(gfs, false);
    inode = dfs_getInode(dfs_getfs(gfs, dfs_getFsHandle(fi->fh)),
                         dfs_getInodeHandle(fi->fh), true, true);
    if (inode == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, path, fi->fh, ENOENT);
        return -ENOENT;
    }
    dtruncate(inode, off);
    dfs_unlock(gfs);
    return 0;
}

/* Get attributes of a file handle */
static int
dfs_fgetattr(const char *path, struct stat *buf, struct fuse_file_info *fi) {
    struct gfs *gfs = getfs();
    struct inode *inode;

    dfs_displayEntry(__func__, path, fi->fh);
    dfs_lock(gfs, false);
    inode = dfs_getInode(dfs_getfs(gfs, dfs_getFsHandle(fi->fh)),
                         dfs_getInodeHandle(fi->fh), false, false);
    if (inode == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, path, fi->fh, ENOENT);
        return -ENOENT;
    }
    memcpy(buf, &inode->i_stat, sizeof(struct stat));
    dfs_inodeUnlock(inode);
    dfs_unlock(gfs);
    return 0;
}

/*
static int
dfs_flock(const char *path, struct fuse_file_info *fi, int cmd,
         struct flock *lock) {
    dfs_displayEntry(__func__, path, fi->fh);
    return 0;
}
*/

/* Update times on a file */
static int
dfs_utimens(const char *path, const struct timespec tv[2]) {
    struct gfs *gfs = getfs();
    struct inode *inode;

    dfs_displayEntry(__func__, path, 0);
    dfs_lock(gfs, false);
    inode = dfs_getPathInode(path, gfs, true, true);
    if (inode == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, path, 0, ENOENT);
        return -ENOENT;
    }
    inode->i_stat.st_atime = tv[0].tv_sec;
    inode->i_stat.st_mtime = tv[1].tv_sec;
    dfs_updateInodeTimes(inode, false, true, true);
    dfs_inodeUnlock(inode);
    dfs_unlock(gfs);
    return 0;
}

/*
static int
dfs_bmap(const char *path, size_t blocksize, uint64_t *idx) {
    dfs_displayEntry(__func__, path, 0);
    return 0;
}

static int
dfs_ioctl(const char *path, int cmd, void *arg,
          struct fuse_file_info *fi, unsigned int flags, void *data) {
    dfs_displayEntry(__func__, path, 0);
    return 0;
}

static int
dfs_poll(const char *path, struct fuse_file_info *fi,
         struct fuse_pollhandle *ph, unsigned *reventsp) {
    dfs_displayEntry(__func__, path, 0);
    return 0;
}

static int
dfs_write_buf(const char *path, struct fuse_bufvec *buf, off_t off,
              struct fuse_file_info *fi) {
    dfs_displayEntry(__func__, path, fi->fh);
    struct fuse_bufvec dst = FUSE_BUFVEC_INIT(fuse_buf_size(buf));

    dfs_lock(gfs, false);
    dst.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
    dst.buf[0].fd = fi->fh;
    dst.buf[0].pos = off;
    dfs_unlock(gfs);

    return fuse_buf_copy(&dst, buf, FUSE_BUF_SPLICE_NONBLOCK);
}

static int
dfs_read_buf(const char *path, struct fuse_bufvec **bufp,
             size_t size, off_t off, struct fuse_file_info *fi) {
    dfs_displayEntry(__func__, path, fi->fh);
    dfs_lock(gfs, false);
    struct fuse_bufvec *src = malloc(sizeof(struct fuse_bufvec));
    struct inode *inode = dfs_getInode(getfs(), fi->fh, false, false);
    if (inode == NULL) {
        dfs_unlock(gfs);
        dfs_reportError(__func__, path, fi->fh, ENOENT);
        return -ENOENT;
    }
    size_t fsize = inode->i_stat.st_size;
    off_t endoffset;

    dfs_inodeUnlock(inode);
    if (src == NULL) {
        dfs_reportError(__func__, path, fi->fh, ENOMEM);
        return -ENOMEM;
    }
    endoffset = off + size;
    if (endoffset > fsize) {
        endoffset = fsize;
    }
    *src = FUSE_BUFVEC_INIT(endoffset - off);
    src->buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
    src->buf[0].fd = fi->fh;
    src->buf[0].pos = off;
    *bufp = src;
    dfs_unlock(gfs);
    return 0;
}

static int
dfs_flock(const char *path, struct fuse_file_info *fi, int op) {
    dfs_displayEntry(__func__, path, fi->fh);
    return 0;
}

static int
dfs_fallocate(const char *path, int mode, off_t offset, off_t length,
              struct fuse_file_info *fi) {
    dfs_displayEntry(__func__, path, fi->fh);
    return 0;
}

*/

struct gfs *gfs = NULL;

/* Initialize a new file system */
static void *
dfs_init(struct fuse_conn_info *conn) {
    printf("%s: gfs %p\n", __func__, gfs);
    return gfs;
}

/* Destroy a file system */
static void
dfs_destroy(void *fsp) {
    struct gfs *gfs = (struct gfs *)fsp;

    printf("%s: gfs %p\n", __func__, gfs);
    close(gfs->gfs_fd);
    if (gfs->gfs_super != NULL) {
        free(gfs->gfs_super);
    }
    pthread_mutex_destroy(&gfs->gfs_ilock);
    pthread_rwlock_destroy(&gfs->gfs_rwlock);
}

/* Fuse operations registered with the fuse driver */
static struct fuse_operations dfs_oper = {
	.getattr	= dfs_getattr,
	.readlink	= dfs_readlink,
	//.getdir 	= dfs_getdir,
	.mknod  	= dfs_mknod,
	.mkdir  	= dfs_mkdir,
	.unlink  	= dfs_unlink,
	.rmdir		= dfs_rmdir,
	.symlink	= dfs_symlink,
    .rename     = dfs_rename,
    .link       = dfs_link,
    .chmod      = dfs_chmod,
    .chown      = dfs_chown,
    .truncate   = dfs_truncate,
    //.utime      = dfs_utime,
    .open       = dfs_open,
    .read       = dfs_read,
    .write      = dfs_write,
    .statfs     = dfs_statfs,
    .flush      = dfs_flush,
    .release    = dfs_release,
    .fsync      = dfs_fsync,
    .setxattr   = dfs_setxattr,
    //.getxattr   = dfs_getxattr,
    .listxattr  = dfs_listxattr,
    .removexattr  = dfs_removexattr,
    .opendir    = dfs_opendir,
    .readdir    = dfs_readdir,
    .releasedir = dfs_releasedir,
    //.fsyncdir   = dfs_fsyncdir,
    .init       = dfs_init,
    .destroy    = dfs_destroy,
    //.access     = dfs_access,
    .create     = dfs_create,
    .ftruncate  = dfs_ftruncate,
    .fgetattr   = dfs_fgetattr,
    //.lock       = dfs_flock,
    .utimens    = dfs_utimens,
    //.bmap       = dfs_bmap,
    //.ioctl      = dfs_ioctl,
    //.poll       = dfs_poll,
    //.write_buf  = dfs_write_buf,
    //.read_buf   = dfs_read_buf,
    //.flock      = dfs_flock,
    //.fallocate  = dfs_fallocate,
};

/* Open the file system device(s) and mount it with fuse */
int
main(int argc, char *argv[]) {
    char *arg[ARGC];
    struct fs *fs;
    size_t size;
    int fd, err;

    if (argc != 3) {
        printf("%s: device mnt\n", argv[0]);
        exit(EINVAL);
    }
    fd = open(argv[1], O_RDWR | O_SYNC | O_DIRECT | O_EXCL, 0);
    if (fd == -1) {
        perror("open");
        exit(errno);
    }
    size = lseek(fd, 0, SEEK_END);
    if (size == -1) {
        perror("lseek");
        exit(errno);
    }
    gfs = malloc(sizeof(struct gfs));
    pthread_mutex_init(&gfs->gfs_ilock, NULL);
    pthread_rwlock_init(&gfs->gfs_rwlock, NULL);
    gfs->gfs_fd = fd;

    err = dfs_superRead(gfs);
    if (err != 0) {
        printf("Superblock read failed, err %d\n", err);
        exit(err);
    }
    if (gfs->gfs_super->sb_version != DFS_VERSION) {
        dfs_format(gfs, size);
    } else {
        gfs->gfs_super->sb_mounts++;
    }

    fs = malloc(sizeof(struct fs));
    memset(fs, 0, sizeof(struct fs));
    fs->fs_root = DFS_ROOT_INODE;
    fs->fs_gfs = gfs;
    gfs->gfs_fs = fs;
    err = dfs_readInodes(fs);
    if (err != 0) {
        printf("Reading inodes failed, err %d\n", err);
        exit(err);
    }

    err = dfs_superWrite(gfs);
    if (err != 0) {
        printf("Superblock write failed, err %d\n", err);
        exit(err);
    }
    arg[0] = argv[0];
    arg[1] = argv[2];
    arg[2] = "-o";
    arg[3] = malloc(1024);
    sprintf(arg[3],
            "allow_other,auto_unmount,"
            "subtype=dfs,fsname=%s,big_writes,"
            "splice_move,splice_read,splice_write,"
            "use_ino,nopath,atomic_o_trunc",
            argv[1]);
    if (ARGC >= 5) {
        arg[4] = "-f";
    }
    if (ARGC >= 6) {
        arg[5] = "-d";
    }
	return fuse_main(ARGC, arg, &dfs_oper, NULL);
}
