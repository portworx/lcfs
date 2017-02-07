#include "includes.h"

static void lc_addName(struct fs *fs, struct cdir *cdir, ino_t ino, char *name,
                       mode_t mode, uint16_t len, ino_t lastIno,
                       enum lc_changeType ctype);

/* Return the type of changed based on inode number */
static inline enum lc_changeType
lc_changeInode(ino_t ino, ino_t lastIno) {
    return (ino > lastIno) ? LC_ADDED : LC_MODIFIED;
}

/* Add a file to the change list */
static void
lc_addFile(struct fs *fs, struct cdir *cdir, ino_t ino, char *name,
           uint16_t len, enum lc_changeType ctype) {
    struct cfile *cfile = cdir->cd_file, **prev = &cdir->cd_file;

    //lc_printf("Adding file %s inode %ld, path %s type %d\n", name, ino, cdir->cd_path, ctype);
    assert(cdir->cd_type != LC_REMOVED);

    /* Check if the file already in the list */
    while (cfile &&
           ((cfile->cf_len != len) || strncmp(cfile->cf_name, name, len))) {
        prev = &cfile->cf_next;
        cfile = cfile->cf_next;
    }

    /* If an entry exists already, return after updating it */
    if (cfile && (cfile->cf_len == len) &&
        !strncmp(cfile->cf_name, name, len)) {
        assert(cfile->cf_type == LC_REMOVED);
        assert(ctype == LC_ADDED);
        cfile->cf_type = LC_MODIFIED;
        return;
    }

    /* Create a new entry and add at the end of the list */
    cfile = lc_malloc(fs, sizeof(struct cfile), LC_MEMTYPE_CFILE);
    cfile->cf_type = ctype;
    cfile->cf_name = name;
    cfile->cf_len = len;
    cfile->cf_next = NULL;
    *prev = cfile;
}

/* Compare directory entries with parent layer and populate the change list
 * with changes in the directory.
 */
static void
lc_processDirectory(struct fs *fs, struct inode *dir, struct inode *pdir,
                    ino_t lastIno, struct cdir *cdir) {
    struct dirent *dirent, *pdirent, *fdirent, *ldirent, *adirent;
    bool hashed = (dir->i_flags & LC_INODE_DHASHED);
    int i, max;

    assert(dir->i_fs == fs);
    assert(!(dir->i_flags & LC_INODE_SHARED));

    /* Traverse parent directory entries looking for missing entries */
    if (hashed) {
        assert((pdir == NULL) || (pdir->i_flags & LC_INODE_DHASHED));
        max = LC_DIRCACHE_SIZE;
    } else {
        assert((pdir == NULL) || !(pdir->i_flags & LC_INODE_DHASHED));
        max = 1;
    }
    for (i = 0; i < max; i++) {
        if (hashed) {
            pdirent = pdir ? pdir->i_hdirent[i] : NULL;
            dirent = dir->i_hdirent[i];
        } else {
            pdirent = pdir ? pdir->i_dirent : NULL;
            dirent = dir->i_dirent;
        }
        fdirent = dirent;
        adirent = NULL;

        /* Directory entries have the same order in both layers */
        while (pdirent) {
            ldirent = dirent;
            while (dirent && (dirent->di_ino != pdirent->di_ino)) {
                dirent = dirent->di_next;
            }

            /* Check if the file was renamed */
            if (dirent) {
                if (adirent == NULL) {
                    adirent = dirent;
                }
                assert(dirent->di_ino == pdirent->di_ino);
                if ((dirent->di_size != pdirent->di_size) ||
                    (strcmp(pdirent->di_name, dirent->di_name))) {
                    lc_addName(fs, cdir, pdirent->di_ino, pdirent->di_name,
                               pdirent->di_mode, pdirent->di_size,
                               lastIno, LC_REMOVED);
                    lc_addName(fs, cdir, dirent->di_ino, dirent->di_name,
                               dirent->di_mode, dirent->di_size, lastIno,
                               LC_ADDED);
                }
                dirent = dirent->di_next;
            } else {

                /* If the entry is not present in the layer, add a record for
                 * the removed file.
                 */
                lc_addName(fs, cdir, pdirent->di_ino, pdirent->di_name,
                           pdirent->di_mode, pdirent->di_size,
                           lastIno, LC_REMOVED);
                dirent = ldirent;
            }
            pdirent = pdirent->di_next;
        }


        /* Process any newly created entries */
        dirent = fdirent;
        while (dirent != adirent) {
            lc_addName(fs, cdir, dirent->di_ino, dirent->di_name,
                       dirent->di_mode, dirent->di_size, lastIno, LC_ADDED);
            dirent = dirent->di_next;
        }
    }
}

/* Lookup the inode number corresponding to the path */
static struct inode *
lc_pathLookup(struct fs *fs, char *path, uint16_t len) {
    struct inode *dir = fs->fs_rootInode;
    ino_t ino = LC_INVALID_INODE;
    char *name = alloca(len);
    uint16_t i = 1, j = 0;

    assert(path[0] == '/');

    /* Break up the path into components and lookup to see if each of those
     * present in the layer.
     */
    while (i < len) {
        if (path[i] == '/') {
            if (j) {
                name[j] = 0;
                j = 0;
                ino = lc_dirLookup(fs, dir, name);
                dir = (ino == LC_INVALID_INODE) ? NULL :
                    lc_getInode(fs, ino, NULL, false, false);
                if ((dir == NULL) || !S_ISDIR(dir->i_mode)) {
                    break;
                }
            }
        } else {
            name[j++] = path[i];
        }
        i++;
    }
    if (j) {
        name[j] = 0;
        ino = lc_dirLookup(fs, dir, name);
        dir = (ino == LC_INVALID_INODE) ? NULL :
                                lc_getInode(fs, ino, NULL, false, false);
    }
    return (dir && S_ISDIR(dir->i_mode)) ? dir : NULL;
}

/* Compare a newly created directory with directory in the parent layer with
 * same path.
 */
static void
lc_compareDirectory(struct fs *fs, struct inode *dir, struct inode *pdir,
                    ino_t lastIno, struct cdir *cdir) {
    bool hashed = (dir->i_flags & LC_INODE_DHASHED);
    int i, max = hashed ? LC_DIRCACHE_SIZE : 1;
    ino_t ino = LC_INVALID_INODE;
    struct dirent *dirent;
    uint64_t count = 0;

    if (pdir && ((dir == fs->fs_rootInode) || (pdir->i_ino == dir->i_ino)) &&
        ((dir->i_flags & LC_INODE_DHASHED) ==
         (pdir->i_flags & LC_INODE_DHASHED))) {
        lc_processDirectory(fs, dir, pdir, lastIno, cdir);
        return;
    }
    /* Check for entries currently present */
    for (i = 0; i < max; i++) {
        dirent = hashed ? dir->i_hdirent[i] : dir->i_dirent;
        while (dirent) {
            if (pdir) {
                ino = lc_dirLookup(fs, pdir, dirent->di_name);
            }
            lc_addName(fs, cdir, dirent->di_ino, dirent->di_name,
                       dirent->di_mode, dirent->di_size, lastIno,
                       (ino == LC_INVALID_INODE) ? LC_ADDED : LC_MODIFIED);
            count++;
            dirent = dirent->di_next;
        }
        if (count == dir->i_size) {
            break;
        }
    }
    if (pdir == NULL) {
        return;
    }

    /* Check missing entries */
    hashed = (pdir->i_flags & LC_INODE_DHASHED);
    max = hashed ? LC_DIRCACHE_SIZE : 1;
    count = 0;
    for (i = 0; i < max; i++) {
        dirent = hashed ? pdir->i_hdirent[i] : pdir->i_dirent;
        while (dirent) {
            ino = lc_dirLookup(fs, dir, dirent->di_name);
            if (ino == LC_INVALID_INODE) {
                lc_addName(fs, cdir, dirent->di_ino, dirent->di_name,
                           dirent->di_mode, dirent->di_size, lastIno,
                           LC_REMOVED);
            }
            count++;
            dirent = dirent->di_next;
        }
        if (count == pdir->i_size) {
            break;
        }
    }
}

/* Add the whole directory tree to the change list */
static void
lc_addDirectoryTree(struct fs *fs, struct inode *dir, struct cdir *cdir,
                    struct cdir *pcdir, ino_t lastIno) {
    ino_t parent = dir->i_dinode.di_parent;
    struct inode *pdir;

    /* Check if an old directory is replaced with a new one.  In that case,
     * compare those directories.
     */
    if (pcdir == NULL) {
        pcdir = fs->fs_changes;
        while (pcdir && (pcdir->cd_ino != parent)) {
            pcdir = pcdir->cd_next;
        }
    }
    if (pcdir->cd_type == LC_MODIFIED) {
        pdir = (dir == fs->fs_rootInode) ? fs->fs_parent->fs_rootInode :
                    lc_pathLookup(fs->fs_parent, cdir->cd_path, cdir->cd_len);
        if (pdir) {
            cdir->cd_type = LC_MODIFIED;
            if (pdir->i_size) {
                lc_compareDirectory(fs, dir, pdir, lastIno, cdir);
                return;
            }
        }
    }

    /* Add everything from the new directory */
    lc_compareDirectory(fs, dir, NULL, lastIno, cdir);
}

/* Find directory entry with the given inode number */
static struct dirent *
lc_getDirent(struct fs *fs, ino_t parent, ino_t ino) {
    struct inode * dir = lc_getInode(fs, parent, NULL, false, false);
    bool hashed = (dir->i_flags & LC_INODE_DHASHED);
    int i, max = hashed ? LC_DIRCACHE_SIZE : 1;
    struct dirent *dirent;

    for (i = 0; i < max; i++) {
        dirent = hashed ? dir->i_hdirent[i] : dir->i_dirent;
        while (dirent) {
            if (dirent->di_ino == ino) {
                i = max;
                break;
            }
            dirent = dirent->di_next;
        }
    }
    lc_inodeUnlock(dir);
    return dirent;
}

/* Add the directory to the change list */
static void
lc_addDirectoryPath(struct fs *fs, ino_t ino, ino_t parent, struct cdir *new,
                    struct cdir *cdir, char *name, uint16_t len) {
    struct cfile *cfile, **prev;
    struct dirent *dirent;
    uint16_t plen;

    /* Root directory is added first */
    if (ino == fs->fs_root) {
        assert(fs->fs_changes == NULL);
        fs->fs_changes = new;
        new->cd_next = NULL;
        new->cd_len = 1;
        new->cd_path = lc_malloc(fs, 1, LC_MEMTYPE_PATH);
        new->cd_path[0] = '/';
    } else {

        /* Find parent directory entry */
        if (cdir == NULL) {
            cdir = fs->fs_changes;
            while (cdir && (cdir->cd_ino != parent)) {
                cdir = cdir->cd_next;
            }
        }
        assert(cdir->cd_ino == parent);

        /* Add the directory after the parent */
        new->cd_next = cdir->cd_next;
        cdir->cd_next = new;

        /* Lookup name if not known */
        if (len == 0) {
            dirent = lc_getDirent(fs, parent, ino);
            name = dirent->di_name;
            len = dirent->di_size;
        }

        /* Check if there is a removed entry for this name */
        if (cdir->cd_type == LC_MODIFIED) {
            cfile = cdir->cd_file;
            prev = &cdir->cd_file;
            while (cfile && ((cfile->cf_len != len) ||
                             strncmp(cfile->cf_name, name, len))) {
                prev = &cfile->cf_next;
                cfile = cfile->cf_next;
            }
            if (cfile && (cfile->cf_len == len) &&
                !strncmp(cfile->cf_name, name, len)) {
                assert(new->cd_type == LC_ADDED);
                assert(cfile->cf_type == LC_REMOVED);
                *prev = cfile->cf_next;
                lc_free(fs, cfile, sizeof(struct cfile), LC_MEMTYPE_CFILE);
                new->cd_type = LC_MODIFIED;
            }
        }

        /* Prepare complete path and link to the record */
        plen = (cdir->cd_len > 1) ? cdir->cd_len : 0;
        new->cd_len = plen + len + 1;
        new->cd_path = lc_malloc(fs, new->cd_len, LC_MEMTYPE_PATH);
        if (plen) {
            memcpy(new->cd_path, cdir->cd_path, plen);
        }
        new->cd_path[plen] = '/';
        memcpy(&new->cd_path[plen + 1], name, len);
    }
}

/* Add a directory to the change list */
static struct cdir *
lc_addDirectory(struct fs *fs, struct inode *dir, char *name, uint16_t len,
                ino_t lastIno, enum lc_changeType ctype) {
    ino_t ino = dir->i_ino, parent = dir->i_dinode.di_parent;
    struct cdir *cdir, *new, *pcdir = NULL;
    struct inode *pdir;
    bool path = true;

    //lc_printf("Directory %ld parent %ld ctype %d\n", ino, parent, ctype);
    if ((dir->i_fs != fs) && (dir->i_fs->fs_root == parent)) {
        parent = fs->fs_root;
    }

retry:

    /* Check if the directory entry exists already */
    cdir = pcdir ? pcdir : fs->fs_changes;
    while (cdir && (cdir->cd_ino != ino) && (cdir->cd_parent != parent)) {
        cdir = cdir->cd_next;
    }

    /* Check if an entry is found */
    if (cdir && (cdir->cd_ino == ino) && (cdir->cd_parent == parent)) {
        new = cdir;
        goto out;
    }
    assert(!(dir->i_flags & LC_INODE_CTRACKED));

    /* Add all directories in the path */
    if ((ino != parent) && path) {
        pdir = lc_getInode(fs, parent, NULL, false, false);
        if (!(pdir->i_flags & LC_INODE_CTRACKED)) {
            pcdir = lc_addDirectory(fs, pdir, NULL, 0, lastIno,
                                    lc_changeInode(pdir->i_ino, lastIno));
        }
        lc_inodeUnlock(pdir);
        path = false;
        goto retry;
    }

    /* Create a new entry for this directory */
    new = lc_malloc(fs, sizeof(struct cdir), LC_MEMTYPE_CDIR);
    new->cd_ino = ino;
    new->cd_type = ctype;
    new->cd_parent = parent;
    new->cd_file = NULL;

    /* Add this directory to the change list */
    lc_addDirectoryPath(fs, ino, parent, new, pcdir, name, len);

out:
    if ((dir->i_fs == fs) && !(dir->i_flags & LC_INODE_CTRACKED)) {
        dir->i_flags |= LC_INODE_CTRACKED;

        /* Add the complete directory tree */
        lc_addDirectoryTree(fs, dir, new, pcdir, lastIno);
    }
    return new;
}

/* Add an inode to the change list */
static void
lc_addInode(struct fs *fs, struct inode *inode, ino_t lastIno) {
    ino_t parent = inode->i_dinode.di_parent;
    struct cdir *cdir = fs->fs_changes;
    ino_t ino = inode->i_ino;
    struct dirent *dirent;
    struct inode *dir;

    /* XXX Take care of inodes with hardlinks */
    assert(S_ISDIR(inode->i_mode) || (inode->i_nlink == 1) ||
           (inode->i_ino > lastIno));
    assert(!(inode->i_flags & LC_INODE_CTRACKED));

    /* Find the entry for parent directory */
    while (cdir && (cdir->cd_ino != parent)) {
        cdir = cdir->cd_next;
    }

    /* If an entry for the parent doesn't exist, add one */
    if (cdir == NULL) {
        dir = lc_getInode(fs, parent, NULL, false, false);
        assert(dir->i_ino < lastIno);
        cdir = lc_addDirectory(fs, dir, NULL, 0, lastIno, LC_MODIFIED);
        lc_inodeUnlock(dir);
    }
    assert(cdir->cd_ino == parent);
    assert(!(inode->i_flags & LC_INODE_CTRACKED));

    /* XXX Take care of inodes with hardlinks */
    dirent = lc_getDirent(fs, parent, ino);
    lc_addFile(fs, cdir, ino, dirent->di_name, dirent->di_size,
               lc_changeInode(ino, lastIno));
    inode->i_flags |= LC_INODE_CTRACKED;
}

/* Add a record to the change list */
static void
lc_addName(struct fs *fs, struct cdir *cdir, ino_t ino, char *name,
           mode_t mode, uint16_t len, ino_t lastIno,
           enum lc_changeType ctype) {
    struct inode *dir, *inode;

    //lc_printf("Adding name %s ino %ld mode %d type %d\n", name, ino, mode, ctype);
    if (S_ISDIR(mode) && (ctype != LC_REMOVED)) {
        dir = lc_getInode(fs, ino, NULL, false, false);
        if (!(dir->i_flags & LC_INODE_CTRACKED) || (ctype == LC_ADDED)) {
            lc_addDirectory(fs, dir, name, len, lastIno, ctype);
        }
        lc_inodeUnlock(dir);
    } else {
        lc_addFile(fs, cdir, ino, name, len, ctype);

        /* Flag the inode as tracked in change list */
        if (ctype != LC_REMOVED) {
            inode = lc_lookupInode(fs, ino);
            if (inode) {
                assert(inode->i_fs == fs);
                inode->i_flags |= LC_INODE_CTRACKED;
            }
        }
    }
}

/* Produce diff between a layer and its parent layer */
void
lc_layerDiff(fuse_req_t req, const char *name, size_t size) {
    struct inode *inode;
    struct fs *fs, *rfs;
    ino_t ino, lastIno;
    int i;

    assert(size == LC_BLOCK_SIZE);
    rfs = lc_getLayerLocked(LC_ROOT_INODE, false);
    ino = lc_getRootIno(rfs, name, NULL, true);
    fs = lc_getLayerLocked(ino, true);
    assert(fs->fs_root == lc_getInodeHandle(ino));
    if (fs->fs_removed) {
        lc_unlock(fs);
        lc_unlock(rfs);
        fuse_reply_err(req, EIO);
        return;
    }
    lastIno = fs->fs_parent->fs_super->sb_lastInode;

    /* Add the root inode to the change list first */
    lc_addDirectory(fs, fs->fs_rootInode, NULL, 0, lastIno, LC_MODIFIED);

    /* Traverse inode cache, looking for modified directories in this layer */
    for (i = 0; i < fs->fs_icacheSize; i++) {
        inode = fs->fs_icache[i].ic_head;
        while (inode) {

            /* Skip removed directories and those already processed */
            if (S_ISDIR(inode->i_mode) &&
                !(inode->i_flags & (LC_INODE_REMOVED | LC_INODE_CTRACKED))) {
                lc_addDirectory(fs, inode, NULL, 0, lastIno,
                                lc_changeInode(inode->i_ino, lastIno));
            }
            inode = inode->i_cnext;
        }
    }

    /* Traverse inode cache, looking for modified files in this layer */
    for (i = 0; i < fs->fs_icacheSize; i++) {
        inode = fs->fs_icache[i].ic_head;
        while (inode) {

            /* Skip removed files and those already processed */
            if (!(inode->i_flags & (LC_INODE_REMOVED | LC_INODE_CTRACKED)) &&
                !S_ISDIR(inode->i_mode)) {
                lc_addInode(fs, inode, lastIno);
            }
            inode = inode->i_cnext;
        }
    }
    lc_unlock(fs);
    lc_unlock(rfs);
}

/* Free the list created for tracking changes in the layer */
void
lc_freeChangeList(struct fs *fs) {
    struct cdir *cdir = fs->fs_changes, *dir;
    struct cfile *cfile, *file;

    while (cdir) {
        cfile = cdir->cd_file;
        while (cfile) {
            file = cfile;
            cfile = cfile->cf_next;
            lc_free(fs, file, sizeof(struct cfile), LC_MEMTYPE_CFILE);
        }
        if (cdir->cd_path) {
            lc_free(fs, cdir->cd_path, cdir->cd_len, LC_MEMTYPE_PATH);
        }
        dir = cdir;
        cdir = cdir->cd_next;
        lc_free(fs, dir, sizeof(struct cdir), LC_MEMTYPE_CDIR);
    }
    fs->fs_changes = NULL;
}

