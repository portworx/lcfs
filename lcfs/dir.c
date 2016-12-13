#include "includes.h"

/* Calculate hash value for the name */
static uint32_t
lc_dirhash(const char *name, size_t size) {
    size_t i, hsize = size > LC_DIRHASH_LEN ? LC_DIRHASH_LEN : size;
    uint32_t hash = 0;

    for (i = 0; i < hsize; i++) {
        hash += name[i];
    }
    return (hash + size) % LC_DIRCACHE_SIZE;
}

/* Allocate hash table for an inode */
void
lc_dirConvertHashed(struct fs *fs, struct inode *dir) {
    struct dirent *dirent = dir->i_dirent, *next, **dcache;
    uint32_t hash;

    assert(S_ISDIR(dir->i_mode));
    dcache = lc_malloc(fs, LC_DIRCACHE_SIZE * sizeof(struct dirent *),
                       LC_MEMTYPE_DCACHE);
    memset(dcache, 0, LC_DIRCACHE_SIZE * sizeof(struct dirent *));
    while (dirent) {
        next = dirent->di_next;
        hash = lc_dirhash(dirent->di_name, dirent->di_size);
        dirent->di_next = dcache[hash];
        dcache[hash] = dirent;
        /* XXX readdir may break */
        dirent->di_index = dirent->di_next ?
                           (dirent->di_next->di_index + 1) : 1;
        dirent = next;
    }
    dir->i_hdirent = dcache;
    dir->i_flags |= LC_INODE_DHASHED;
    //lc_printf("Converted to hashed directory %ld\n", dir->i_ino);
}

/* Lookup the specified name in the directory and return correponding inode
 * number if found.
 */
ino_t
lc_dirLookup(struct fs *fs, struct inode *dir, const char *name) {
    struct dirent *dirent;
    int len = strlen(name);
    uint32_t hash;
    ino_t dino;

    assert(S_ISDIR(dir->i_mode));
    if (dir->i_flags & LC_INODE_DHASHED) {
        hash = lc_dirhash(name, len);
        dirent = dir->i_hdirent[hash];
    } else {
        dirent = dir->i_dirent;
    }
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
    struct dirent *dirent;
    int hash;

    assert(S_ISDIR(dir->i_mode));
    assert(!(dir->i_flags & LC_INODE_SHARED));
    assert(ino > LC_ROOT_INODE);

    /* Convert to a hash table when the directory grows bigger than a certain
     * size.
     * XXX Disable this for now.
     */
    if (0 && (dir->i_size >= LC_DIRCACHE_MIN) &&
        !(dir->i_flags & LC_INODE_DHASHED)) {
        lc_dirConvertHashed(fs, dir);
    }
    dirent = lc_malloc(fs, sizeof(struct dirent) + nsize + 1,
                       LC_MEMTYPE_DIRENT);
    dirent->di_ino = ino;
    dirent->di_name = ((char *)dirent) + sizeof(struct dirent);
    memcpy(dirent->di_name, name, nsize);
    dirent->di_name[nsize] = 0;
    dirent->di_size = nsize;
    dirent->di_mode = mode & S_IFMT;
    if (dir->i_flags & LC_INODE_DHASHED) {
        hash = lc_dirhash(name, nsize);
        dirent->di_next = dir->i_hdirent[hash];
        dir->i_hdirent[hash] = dirent;
    } else {
        dirent->di_next = dir->i_dirent;
        dir->i_dirent = dirent;
    }
    dirent->di_index = dirent->di_next ? (dirent->di_next->di_index + 1) : 1;
    dir->i_size++;
}

/* Copy directory entries from one directory to another */
void
lc_dirCopy(struct inode *dir) {
    bool hashed = (dir->i_flags & LC_INODE_DHASHED);
    struct dirent *dirent, *new, *prev, **dcache;
    struct fs *fs = dir->i_fs;
    uint64_t count = 0;
    uint32_t i, max;
    size_t nsize;

    assert(dir->i_flags & LC_INODE_SHARED);
    assert(S_ISDIR(dir->i_mode));
    assert(dir->i_nlink >= 2);
    if (hashed) {
        dcache = dir->i_hdirent;
        dir->i_hdirent = NULL;
        lc_dirConvertHashed(fs, dir);
        max = LC_DIRCACHE_SIZE;
        dirent = NULL;
    } else {
        dirent = dir->i_dirent;
        dir->i_dirent = NULL;
        max = 1;
        dcache = NULL;
    }
    dir->i_flags &= ~LC_INODE_SHARED;
    for (i = 0; i < max; i++) {
        if (hashed) {
            dirent = dcache[i];
        }
        prev = NULL;
        while (dirent) {
            nsize = dirent->di_size;
            new = lc_malloc(fs, sizeof(struct dirent) + nsize + 1,
                            LC_MEMTYPE_DIRENT);
            new->di_ino = dirent->di_ino;
            new->di_name = ((char *)new) + sizeof(struct dirent);
            memcpy(new->di_name, dirent->di_name, nsize);
            new->di_name[nsize] = 0;
            new->di_size = nsize;
            new->di_mode = dirent->di_mode;
            new->di_index = dirent->di_index;
            new->di_next = NULL;
            if (prev == NULL) {
                if (hashed) {
                    dir->i_hdirent[i] = new;
                } else {
                    dir->i_dirent = new;
                }
            } else {
                prev->di_next = new;
            }
            count++;
            prev = new;
            dirent = dirent->di_next;
        }
    }
    assert(dir->i_size == count);
    lc_markInodeDirty(dir, true, true, false, false);
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
    bool hashed = (dir->i_flags & LC_INODE_DHASHED);
    struct dirent *dirent, *pdirent = NULL;
    int len = strlen(name);
    uint32_t hash;

    assert(S_ISDIR(dir->i_mode));
    assert(!(dir->i_flags & LC_INODE_SHARED));
    if (hashed) {
        hash = lc_dirhash(name, len);
        dirent = dir->i_hdirent[hash];
    } else {
        hash = -1;
        dirent = dir->i_dirent;
    }
    while (dirent != NULL) {
        if ((len == dirent->di_size) &&
            (strcmp(name, dirent->di_name) == 0)) {
            if (pdirent == NULL) {
                if (hashed) {
                    dir->i_hdirent[hash] = dirent->di_next;
                } else {
                    dir->i_dirent = dirent->di_next;
                }
            } else {
                pdirent->di_next = dirent->di_next;
            }
            dir->i_size--;
            lc_freeDirent(dir->i_fs, dirent);
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
    bool hashed = (dir->i_flags & LC_INODE_DHASHED);
    struct dirent *dirent, *pdirent = NULL, *new;
    int len = strlen(name), hash, newhash = 0;
    struct fs *fs;

    assert(S_ISDIR(dir->i_mode));
    assert(!(dir->i_flags & LC_INODE_SHARED));
    if (hashed) {
        hash = lc_dirhash(name, len);
        dirent = dir->i_hdirent[hash];
    } else {
        dirent = dir->i_dirent;
        hash = -1;
    }
    while (dirent != NULL) {
        if ((dirent->di_ino == ino) && (len == dirent->di_size) &&
            (strcmp(name, dirent->di_name) == 0)) {
            fs = dir->i_fs;
            len = strlen(newname);
            if (hashed) {
                newhash = lc_dirhash(newname, len);
                if (hash != newhash) {
                    if (pdirent == NULL) {
                        dir->i_hdirent[hash] = dirent->di_next;
                    } else {
                        pdirent->di_next = dirent->di_next;
                    }
                    dirent->di_next = dir->i_hdirent[newhash];
                    dir->i_hdirent[newhash] = dirent;
                    dirent->di_index = dirent->di_next ?
                                       (dirent->di_next->di_index + 1) : 1;
                    pdirent = NULL;
                }
            }

            /* Existing name can be used if size is not growing */
            if (len > dirent->di_size) {
                new = lc_malloc(fs, sizeof(struct dirent) + len + 1,
                                LC_MEMTYPE_DIRENT);
                memcpy(new, dirent, sizeof(struct dirent));
                lc_freeDirent(fs, dirent);
                dirent = new;
                if (pdirent == NULL) {
                    if (hashed) {
                        dir->i_hdirent[newhash] = dirent;
                    } else {
                        dir->i_dirent = dirent;
                    }
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
    uint64_t block = dir->i_emapDirBlock, entries = 0;
    int remain, dsize, count = 2;
    struct ddirent *ddirent;
    struct dblock *dblock = buf;
    char *dbuf;

    assert(S_ISDIR(dir->i_mode));
    if ((dir->i_size >= LC_DIRCACHE_MIN) &&
        !(dir->i_flags & LC_INODE_DHASHED)) {
        lc_dirConvertHashed(fs, dir);
    }
    dir->i_size = 0;
    while (block != LC_INVALID_BLOCK) {
        lc_addSpaceExtent(gfs, fs, &dir->i_emapDirExtents, block, 1);
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
            entries++;
            dbuf += dsize;
            remain -= dsize;
        }
        block = dblock->db_next;
    }
    assert(dir->i_nlink == count);
    assert(dir->i_size == entries);
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
    uint64_t block = LC_INVALID_BLOCK, count = 0, entries = 0;
    bool hashed = (dir->i_flags & LC_INODE_DHASHED);
    int i, remain = 0, dsize, subdir = 2, max;
    struct dblock *dblock = NULL;
    struct page *page = NULL;
    struct ddirent *ddirent;
    struct dirent *dirent;
    char *dbuf = NULL;

    assert(S_ISDIR(dir->i_mode));
    if (dir->i_flags & LC_INODE_REMOVED) {
        dir->i_flags &= ~LC_INODE_DIRDIRTY;
        return;
    }
    max = hashed ? LC_DIRCACHE_SIZE : 1;
    for (i = 0; i < max; i++) {
        dirent = hashed ? dir->i_hdirent[i] : dir->i_dirent;
        while (dirent) {
            dsize = LC_MIN_DIRENT_SIZE + dirent->di_size;
            if (remain < dsize) {
                if (dblock) {
                    page = lc_dirAddPage(gfs, fs, dblock, remain, page);
                }
                lc_mallocBlockAligned(fs->fs_rfs, (void **)&dblock,
                                      LC_MEMTYPE_DATA);
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
            entries++;
            dbuf += dsize;
            remain -= dsize;
            dirent = dirent->di_next;
        }
    }
    if (dblock) {
        page = lc_dirAddPage(gfs, fs, dblock, remain, page);
    }
    if (count) {
        block = lc_dirFlushBlocks(gfs, fs, page, count);
        lc_replaceMetaBlocks(fs, &dir->i_emapDirExtents, block, count);
    }
    dir->i_emapDirBlock = block;
    assert(dir->i_nlink == subdir);
    assert(dir->i_size == entries);
    dir->i_dinode.di_blocks = count;
    assert(dir->i_flags & LC_INODE_DIRTY);
    dir->i_flags &= ~LC_INODE_DIRDIRTY;
}

/* Free directory entries */
void
lc_dirFree(struct inode *dir) {
    bool hashed = (dir->i_flags & LC_INODE_DHASHED);
    struct dirent *dirent, *tmp;
    struct fs *fs;
    int i, max;

    if (dir->i_flags & LC_INODE_SHARED) {
        dir->i_flags &= ~(LC_INODE_SHARED | LC_INODE_DHASHED);
        dir->i_dirent = NULL;
        return;
    }
    fs = dir->i_fs;
    max = hashed ? LC_DIRCACHE_SIZE : 1;
    for (i = 0; i < max; i++) {
        dirent = hashed ? dir->i_hdirent[i] : dir->i_dirent;
        while (dirent != NULL) {
            tmp = dirent;
            dirent = dirent->di_next;
            lc_freeDirent(fs, tmp);
        }
    }
    if (hashed) {
        lc_free(fs, dir->i_hdirent, LC_DIRCACHE_SIZE * sizeof(struct dirent *),
                LC_MEMTYPE_DCACHE);
        dir->i_hdirent = NULL;
        dir->i_flags &= ~LC_INODE_DHASHED;
    } else {
        dir->i_dirent = NULL;
    }
}

/* Remove a directory tree */
void
lc_removeTree(struct fs *fs, struct inode *dir) {
    bool hashed = (dir->i_flags & LC_INODE_DHASHED);
    struct dirent *dirent;
    int i, max;
    bool rmdir;

    assert(!(dir->i_flags & LC_INODE_SHARED));
    max = hashed ? LC_DIRCACHE_SIZE : 1;
    for (i = 0; i < max; i++) {
        dirent = hashed ?  dir->i_hdirent[i] : dir->i_dirent;
        while (dirent != NULL) {
            lc_printf("lc_removeTree: dir %ld nlink %ld removing %s inode %ld dir %d\n", dir->i_ino, dir->i_nlink, dirent->di_name, dirent->di_ino, S_ISDIR(dirent->di_mode));
            rmdir = S_ISDIR(dirent->di_mode);
            lc_removeInode(fs, dir, dirent->di_ino, rmdir, NULL);
            if (rmdir) {
                assert(dir->i_nlink > 2);
                dir->i_nlink--;
            } else {
                assert(dir->i_nlink >= 2);
            }
            if (hashed) {
                dir->i_hdirent[i] = dirent->di_next;
            } else {
                dir->i_dirent = dirent->di_next;
            }
            dir->i_size--;
            lc_freeDirent(fs, dirent);
            dirent = hashed ? dir->i_hdirent[i] : dir->i_dirent;
        }
    }
}

/* Lookup an entry in the directory and remove that if present */
int
lc_dirRemoveName(struct fs *fs, struct inode *dir,
                 const char *name, bool rmdir, void **fsp,
                 int dremove(struct fs *, struct inode *, ino_t,
                             bool, void **)) {
    bool hashed = (dir->i_flags & LC_INODE_DHASHED);
    struct dirent *dirent, *pdirent = NULL;
    int len = strlen(name), err, hash;
    ino_t ino, parent = dir->i_ino;
    struct gfs *gfs = fs->fs_gfs;

    assert(S_ISDIR(dir->i_mode));
    if (hashed) {
        hash = lc_dirhash(name, len);
        dirent = dir->i_hdirent[hash];
    } else {
        hash = -1;
        dirent = dir->i_dirent;
    }
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
                        assert(dir->i_nlink > 2);
                        dir->i_nlink--;
                    } else {
                        assert(dir->i_nlink >= 2);
                    }
                    lc_updateInodeTimes(dir, false, true);
                } else {
                    err = 0;
                }
                lc_markInodeDirty(dir, true, true, false, false);
                if (pdirent == NULL) {
                    if (hashed) {
                        dir->i_hdirent[hash] = dirent->di_next;
                    } else {
                        dir->i_dirent = dirent->di_next;
                    }
                } else {
                    pdirent->di_next = dirent->di_next;
                }
                dir->i_size--;
                lc_freeDirent(fs, dirent);
            }
            return err;
        }
        pdirent = dirent;
        dirent = dirent->di_next;
    }
    return ENOENT;
}

/* Return directory entries */
void
lc_dirReaddir(fuse_req_t req, struct fs *fs, struct inode *dir,
              fuse_ino_t ino, size_t size, off_t off, struct stat *st) {
    bool hashed = (dir->i_flags & LC_INODE_DHASHED);
    struct dirent *dirent = NULL;
    size_t csize, esize;
    int max, start;
    char buf[size];
    off_t i, hoff;

    assert(S_ISDIR(dir->i_mode));
    if (hashed) {
        max = LC_DIRCACHE_SIZE;
        start = off >> LC_DIRHASH_SHIFT;
        assert(start <= LC_DIRCACHE_SIZE);
        if (start == LC_DIRCACHE_SIZE) {
            start = 0;
        }
    } else {
        start = 0;
        max = 1;
    }
    off &= LC_DIRHASH_INDEX;
    csize = 0;
    for (i = start; i < max; i++) {
        dirent = hashed ?  dir->i_hdirent[i] : dir->i_dirent;
        while (off && dirent && (dirent->di_index >= off)) {
            dirent = dirent->di_next;
        }
        off = 0;
        hoff = (hashed ? i : LC_DIRCACHE_SIZE) << LC_DIRHASH_SHIFT;
        while (dirent != NULL) {
            assert(dirent->di_ino > LC_ROOT_INODE);
            st->st_ino = lc_setHandle(lc_getIndex(fs, ino, dirent->di_ino),
                                      dirent->di_ino);
            st->st_mode = dirent->di_mode;
            esize = fuse_add_direntry(req, &buf[csize], size - csize,
                                      dirent->di_name, st,
                                      hoff | dirent->di_index);
            csize += esize;
            if (csize >= size) {
                csize -= esize;
                goto out;
            }
            dirent = dirent->di_next;
        }
    }

out:
    if (csize) {
        fuse_reply_buf(req, buf, csize);
    } else {
        fuse_reply_buf(req, NULL, 0);
    }
}
