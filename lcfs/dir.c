#include "includes.h"

/* Lookup the specified name in the directory and return correponding inode
 * number if found.
 */
ino_t
lc_dirLookup(struct fs *fs, struct inode *dir, const char *name) {
    struct dirent *dirent;
    int len = strlen(name);
    ino_t dino;

    assert(S_ISDIR(dir->i_stat.st_mode));
    dirent = dir->i_dirent;
    while (dirent != NULL) {
        if ((len == dirent->di_size) &&
            (strcmp(name, dirent->di_name) == 0)) {
            dino = dirent->di_ino;
            return dino;
        }
        dirent = dirent->di_next;
    }
    return LC_INVALID_INODE;
}

/* Add a new directory entry to the given directory */
void
lc_dirAdd(struct inode *dir, ino_t ino, mode_t mode, const char *name,
          int nsize) {
    struct fs *fs = dir->i_fs;
    struct dirent *dirent = lc_malloc(fs, sizeof(struct dirent) + nsize + 1,
                                      LC_MEMTYPE_DIRENT);

    assert(S_ISDIR(dir->i_stat.st_mode));
    assert(!dir->i_shared);
    assert(ino > LC_ROOT_INODE);
    dirent->di_ino = ino;
    dirent->di_name = ((char *)dirent) + sizeof(struct dirent);
    memcpy(dirent->di_name, name, nsize);
    dirent->di_name[nsize] = 0;
    dirent->di_size = nsize;
    dirent->di_mode = mode & S_IFMT;
    dirent->di_next = dir->i_dirent;
    dirent->di_index = dir->i_dirent ? (dir->i_dirent->di_index + 1) : 1;
    dir->i_dirent = dirent;
}

/* Copy directory entries from one directory to another */
void
lc_dirCopy(struct inode *dir) {
    struct dirent *dirent = dir->i_dirent;

    assert(dir->i_shared);
    assert(S_ISDIR(dir->i_stat.st_mode));
    assert(dir->i_stat.st_nlink >= 2);
    dir->i_dirent = NULL;
    dir->i_shared = false;
    while (dirent) {
        lc_dirAdd(dir, dirent->di_ino, dirent->di_mode,
                   dirent->di_name, dirent->di_size);
        dirent = dirent->di_next;
    }
    dir->i_dirdirty = true;
}

/* Free a dirent structure */
static inline void
lc_freeDirent(struct fs *fs, struct dirent *dirent) {
    lc_free(fs, dirent, sizeof(struct dirent) + dirent->di_size + 1,
            LC_MEMTYPE_DIRENT);
}

/* Remove a directory entry */
void
lc_dirRemove(struct inode *dir, const char *name) {
    struct dirent *dirent = dir->i_dirent;
    struct dirent *pdirent = NULL;
    int len = strlen(name);
    struct fs *fs;

    assert(S_ISDIR(dir->i_stat.st_mode));
    assert(!dir->i_shared);
    while (dirent != NULL) {
        if ((len == dirent->di_size) &&
            (strcmp(name, dirent->di_name) == 0)) {
            if (pdirent == NULL) {
                dir->i_dirent = dirent->di_next;
            } else {
                pdirent->di_next = dirent->di_next;
            }
            fs = dir->i_fs;
            lc_freeDirent(fs, dirent);
            return;
        }
        pdirent = dirent;
        dirent = dirent->di_next;
    }
    assert(false);
}

/* Rename a directory entry with a new name */
void
lc_dirRename(struct inode *dir, ino_t ino,
              const char *name, const char *newname) {
    struct dirent *dirent = dir->i_dirent, *pdirent = NULL, *new;
    int len = strlen(name);
    struct fs *fs;

    assert(S_ISDIR(dir->i_stat.st_mode));
    assert(!dir->i_shared);
    while (dirent != NULL) {
        if ((dirent->di_ino == ino) &&(len == dirent->di_size) &&
            (strcmp(name, dirent->di_name) == 0)) {
            fs = dir->i_fs;
            len = strlen(newname);

            /* Existing name can be used if size is not growing */
            if (len > dirent->di_size) {
                new = lc_malloc(fs, sizeof(struct dirent) + len + 1,
                                LC_MEMTYPE_DIRENT);
                memcpy(new, dirent, sizeof(struct dirent));
                lc_freeDirent(fs, dirent);
                dirent = new;
                if (pdirent == NULL) {
                    dir->i_dirent = dirent;
                } else {
                    pdirent->di_next = dirent;
                }
                dirent->di_name = ((char *)dirent) + sizeof(struct dirent);
            } else if (dirent->di_size > len) {
                lc_memUpdateTotal(fs, dirent->di_size - len);
            }
            memcpy(dirent->di_name, newname, len);
            dirent->di_name[len] = 0;
            dirent->di_size = len;
            return;
        }
        pdirent = dirent;
        dirent = dirent->di_next;
    }
    assert(false);
}

/* Read a directory from disk */
void
lc_dirRead(struct gfs *gfs, struct fs *fs, struct inode *dir, void *buf) {
    uint64_t block = dir->i_bmapDirBlock;
    int remain, dsize, count = 2;
    struct ddirent *ddirent;
    struct dblock *dblock = buf;
    char *dbuf;

    assert(S_ISDIR(dir->i_stat.st_mode));
    while (block != LC_INVALID_BLOCK) {
        lc_addExtent(gfs, fs, &dir->i_bmapDirExtents, block, 1);
        lc_readBlock(gfs, fs, block, dblock);
        dbuf = (char *)&dblock->db_dirent[0];
        remain = LC_BLOCK_SIZE - sizeof(struct dblock);
        while (remain > LC_MIN_DIRENT_SIZE) {
            ddirent = (struct ddirent *)dbuf;
            if (ddirent->di_inum == 0) {
                break;
            }
            dsize = LC_MIN_DIRENT_SIZE + ddirent->di_len;
            lc_dirAdd(dir, ddirent->di_inum, ddirent->di_type,
                       ddirent->di_name, ddirent->di_len);
            if (S_ISDIR(ddirent->di_type)) {
                count++;
            }
            dbuf += dsize;
            remain -= dsize;
        }
        block = dblock->db_next;
    }
    assert(dir->i_stat.st_nlink == count);
}

/* Allocate a directory block and flush to disk */
static uint64_t
lc_dirFlushBlocks(struct gfs *gfs, struct fs *fs,
                  struct page *fpage, uint64_t pcount) {
    uint64_t block, count = pcount;
    struct page *page = fpage;
    struct dblock *dblock;

    block = lc_blockAllocExact(fs, pcount, true, true);
    while (page) {
        count--;
        lc_addPageBlockHash(gfs, fs, page, block + count);
        dblock = (struct dblock *)page->p_data;
        dblock->db_next = (page == fpage) ? LC_INVALID_BLOCK :
                                            block + count + 1;
        page = page->p_dnext;
    }
    assert(count == 0);
    lc_flushPageCluster(gfs, fs, fpage, pcount);
    return block;
}

/* Add a new page to the list of directory blocks */
static struct page *
lc_dirAddPage(struct gfs *gfs, struct fs *fs, struct dblock *dblock,
              int remain, struct page *page) {
    char *buf;

    if (remain) {
        buf = (char *)dblock;
        memset(&buf[LC_BLOCK_SIZE - remain], 0, remain);
    }
    return lc_getPageNoBlock(gfs, fs, (char *)dblock, page);
}

/* Flush directory entries */
void
lc_dirFlush(struct gfs *gfs, struct fs *fs, struct inode *dir) {
    uint64_t block = LC_INVALID_BLOCK, count = 0;
    struct dirent *dirent = dir->i_dirent;
    int remain = 0, dsize, subdir = 2;
    struct dblock *dblock = NULL;
    struct page *page = NULL;
    struct ddirent *ddirent;
    char *dbuf = NULL;

    assert(S_ISDIR(dir->i_stat.st_mode));
    if (dir->i_removed) {
        dir->i_dirdirty = false;
        return;
    }
    while (dirent) {
        dsize = LC_MIN_DIRENT_SIZE + dirent->di_size;
        if (remain < dsize) {
            if (dblock) {
                page = lc_dirAddPage(gfs, fs, dblock, remain, page);
            }
            lc_mallocBlockAligned(fs, (void **)&dblock, true);
            dbuf = (char *)&dblock->db_dirent[0];
            remain = LC_BLOCK_SIZE - sizeof(struct dblock);
            count++;
        }

        /* Copy directory entry */
        ddirent = (struct ddirent *)dbuf;
        ddirent->di_inum = dirent->di_ino;
        ddirent->di_type = dirent->di_mode;
        ddirent->di_len = dirent->di_size;
        memcpy(ddirent->di_name, dirent->di_name, ddirent->di_len);
        if (S_ISDIR(dirent->di_mode)) {
            subdir++;
        }
        dbuf += dsize;
        remain -= dsize;
        dirent = dirent->di_next;
    }
    if (dblock) {
        page = lc_dirAddPage(gfs, fs, dblock, remain, page);
    }
    if (count) {
        block = lc_dirFlushBlocks(gfs, fs, page, count);
        lc_replaceMetaBlocks(fs, &dir->i_bmapDirExtents, block, count);
    }
    dir->i_bmapDirBlock = block;
    assert(dir->i_stat.st_nlink == subdir);
    dir->i_stat.st_blocks = count;
    dir->i_stat.st_size = count * LC_BLOCK_SIZE;
    dir->i_dirdirty = false;
    dir->i_dirty = true;
}

/* Free directory entries */
void
lc_dirFree(struct inode *dir) {
    struct dirent *dirent = dir->i_dirent, *tmp;
    struct fs *fs;

    if (dir->i_shared) {
        dir->i_dirent = NULL;
        return;
    }
    fs = dir->i_fs;
    while (dirent != NULL) {
        tmp = dirent;
        dirent = dirent->di_next;
        lc_freeDirent(fs, tmp);
    }
    dir->i_dirent = NULL;
}

/* Remove a directory tree */
void
lc_removeTree(struct fs *fs, struct inode *dir) {
    struct dirent *dirent = dir->i_dirent;
    bool rmdir;

    while (dirent != NULL) {
        lc_printf("lc_removeTree: dir %ld nlink %ld removing %s inode %ld dir %d\n", dir->i_stat.st_ino, dir->i_stat.st_nlink, dirent->di_name, dirent->di_ino, S_ISDIR(dirent->di_mode));
        rmdir = S_ISDIR(dirent->di_mode);
        lc_removeInode(fs, dir, dirent->di_ino, rmdir, NULL);
        if (rmdir) {
            assert(dir->i_stat.st_nlink > 2);
            dir->i_stat.st_nlink--;
        } else {
            assert(dir->i_stat.st_nlink >= 2);
        }
        dir->i_dirent = dirent->di_next;
        lc_freeDirent(fs, dirent);
        dirent = dir->i_dirent;
    }
}

/* Lookup an entry in the directory and remove that if present */
int
lc_dirRemoveName(struct fs *fs, struct inode *dir,
                 const char *name, bool rmdir, void **fsp,
                 int dremove(struct fs *, struct inode *, ino_t,
                             bool, void **)) {
    struct dirent *dirent = dir->i_dirent, *pdirent = NULL;
    ino_t ino, parent = dir->i_stat.st_ino;
    struct gfs *gfs = fs->fs_gfs;
    int len = strlen(name), err;

    assert(S_ISDIR(dir->i_stat.st_mode));
    while (dirent != NULL) {
        if ((len == dirent->di_size) &&
            (strcmp(name, dirent->di_name) == 0)) {
            ino = dirent->di_ino;
            if (rmdir && (fsp == NULL) && (fs->fs_gindex == 0) &&
               ((ino == gfs->gfs_snap_root) ||
                ((gfs->gfs_snap_rootInode != NULL) &&
                 (ino == gfs->gfs_snap_rootInode->i_parent)) ||
                lc_getIndex(fs, parent, ino))) {
                lc_reportError(__func__, __LINE__, parent, EEXIST);
                err = EEXIST;
            } else {
                err = dremove(fs, dir, ino, rmdir, fsp);
            }
            if ((err == 0) || (err == ESTALE)) {
                if (err == 0) {
                    if (rmdir) {
                        assert(dir->i_stat.st_nlink > 2);
                        dir->i_stat.st_nlink--;
                    } else {
                        assert(dir->i_stat.st_nlink >= 2);
                    }
                    lc_updateInodeTimes(dir, false, false, true);
                } else {
                    err = 0;
                }
                lc_markInodeDirty(dir, true, true, false, false);
                if (pdirent == NULL) {
                    dir->i_dirent = dirent->di_next;
                } else {
                    pdirent->di_next = dirent->di_next;
                }
                lc_freeDirent(fs, dirent);
            }
            return err;
        }
        pdirent = dirent;
        dirent = dirent->di_next;
    }
    return ENOENT;
}
