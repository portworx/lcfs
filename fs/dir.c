#include "includes.h"

/* Lookup the specified name in the directory and return correponding inode
 * number if found.
 */
ino_t
dfs_dirLookup(struct fs *fs, struct inode *dir, const char *name) {
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
    return DFS_INVALID_INODE;
}

/* Add a new directory entry to the given directory */
void
dfs_dirAdd(struct inode *dir, ino_t ino, mode_t mode, const char *name,
           int nsize) {
    struct dirent *dirent = malloc(sizeof(struct dirent));

    assert(S_ISDIR(dir->i_stat.st_mode));
    assert(!dir->i_shared);
    assert(ino > DFS_ROOT_INODE);
    dirent->di_ino = ino;
    dirent->di_name = malloc(nsize + 1);
    memcpy(dirent->di_name, name, nsize);
    dirent->di_name[nsize] = 0;
    dirent->di_size = nsize;
    dirent->di_mode = mode & S_IFMT;
    dirent->di_next = dir->i_dirent;
    dir->i_dirent = dirent;
}

/* Copy directory entries from one directory to another */
void
dfs_dirCopy(struct inode *dir) {
    struct dirent *dirent = dir->i_dirent;

    assert(dir->i_shared);
    assert(S_ISDIR(dir->i_stat.st_mode));
    assert(dir->i_stat.st_nlink >= 2);
    dir->i_dirent = NULL;
    dir->i_shared = false;
    while (dirent) {
        dfs_dirAdd(dir, dirent->di_ino, dirent->di_mode,
                   dirent->di_name, dirent->di_size);
        dirent = dirent->di_next;
    }
    dir->i_dirdirty = true;
}


/* Remove a directory entry */
void
dfs_dirRemove(struct inode *dir, const char *name) {
    struct dirent *dirent = dir->i_dirent;
    struct dirent *pdirent = NULL;
    int len = strlen(name);

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
            free(dirent->di_name);
            free(dirent);
            return;
        }
        pdirent = dirent;
        dirent = dirent->di_next;
    }
    assert(false);
}

/* Remove a directory entry by inode number */
void
dfs_dirRemoveInode(struct inode *dir, ino_t ino) {
    struct dirent *dirent = dir->i_dirent;
    struct dirent *pdirent = NULL;

    assert(S_ISDIR(dir->i_stat.st_mode));
    assert(!dir->i_shared);
    while (dirent != NULL) {
        if (dirent->di_ino == ino) {
            if (pdirent == NULL) {
                dir->i_dirent = dirent->di_next;
            } else {
                pdirent->di_next = dirent->di_next;
            }
            free(dirent->di_name);
            free(dirent);
            break;
        }
        pdirent = dirent;
        dirent = dirent->di_next;
    }
}

/* Rename a directory entry with a new name */
void
dfs_dirRename(struct inode *dir, ino_t ino,
              const char *name, const char *newname) {
    struct dirent *dirent = dir->i_dirent;
    int len = strlen(name);

    assert(S_ISDIR(dir->i_stat.st_mode));
    assert(!dir->i_shared);
    while (dirent != NULL) {
        if ((dirent->di_ino == ino) &&(len == dirent->di_size) &&
            (strcmp(name, dirent->di_name) == 0)) {
            len = strlen(newname);

            /* Existing name can be used if size is not growing */
            if (len > dirent->di_size) {
                free(dirent->di_name);
                dirent->di_name = malloc(len + 1);
            }
            memcpy(dirent->di_name, newname, len);
            dirent->di_name[len] = 0;
            dirent->di_size = len;
            return;
        }
        dirent = dirent->di_next;
    }
    assert(false);
}

/* Read a directory from disk */
void
dfs_dirRead(struct gfs *gfs, struct fs *fs, struct inode *dir, void *buf) {
    uint64_t block = dir->i_bmapDirBlock;
    int remain, dsize, count = 2;
    struct ddirent *ddirent;
    struct dblock *dblock = buf;
    char *dbuf;

    //dfs_printf("Reading directory %ld\n", dir->i_stat.st_ino);
    assert(S_ISDIR(dir->i_stat.st_mode));
    while (block != DFS_INVALID_BLOCK) {
        //dfs_printf("Reading directory block %ld\n", block);
        dfs_readBlock(gfs, fs, block, dblock);
        dbuf = (char *)&dblock->db_dirent[0];
        remain = DFS_BLOCK_SIZE - sizeof(struct dblock);
        while (remain > DFS_MIN_DIRENT_SIZE) {
            ddirent = (struct ddirent *)dbuf;
            if (ddirent->di_inum == 0) {
                break;
            }
            dsize = DFS_MIN_DIRENT_SIZE + ddirent->di_len;
            dfs_dirAdd(dir, ddirent->di_inum, ddirent->di_type,
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

/* Flush a directory block */
static uint64_t
dfs_dirFlushBlock(struct gfs *gfs, struct fs *fs, struct dblock *dblock,
                  int remain) {
    uint64_t block = dfs_blockAlloc(fs, 1, true);
    char *buf;

    //dfs_printf("Flushing directory block %ld\n", block);
    if (remain) {
        buf = (char *)dblock;
        memset(&buf[DFS_BLOCK_SIZE - remain], 0, remain);
    }
    dfs_writeBlock(gfs, fs, dblock, block);
    return block;
}

/* Flush directory entries */
void
dfs_dirFlush(struct gfs *gfs, struct fs *fs, struct inode *dir) {
    uint64_t block = DFS_INVALID_BLOCK, count = 0;
    struct dirent *dirent = dir->i_dirent;
    int remain = 0, dsize, subdir = 2;
    struct dblock *dblock = NULL;
    struct ddirent *ddirent;
    char *dbuf = NULL;

    //dfs_printf("Flushing directory %ld\n", dir->i_stat.st_ino);
    assert(S_ISDIR(dir->i_stat.st_mode));
    if (dir->i_removed) {
        dir->i_dirdirty = false;
        return;
    }
    while (dirent) {
        dsize = DFS_MIN_DIRENT_SIZE + dirent->di_size;
        if (remain < dsize) {
            if (dblock) {
                block = dfs_dirFlushBlock(gfs, fs, dblock, remain);
            }
            if (dblock == NULL) {
                posix_memalign((void **)&dblock, DFS_BLOCK_SIZE,
                               DFS_BLOCK_SIZE);
            }
            dblock->db_next = block;
            dbuf = (char *)&dblock->db_dirent[0];
            remain = DFS_BLOCK_SIZE - sizeof(struct dblock);
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
        block = dfs_dirFlushBlock(gfs, fs, dblock, remain);
        free(dblock);
    }
    dir->i_bmapDirBlock = block;
    if (dir->i_stat.st_blocks) {
        /* XXX Free these blocks */
        dfs_blockFree(gfs, dir->i_stat.st_blocks);
    }
    assert(dir->i_stat.st_nlink == subdir);
    dir->i_stat.st_blocks = count;
    dir->i_stat.st_size = count * DFS_BLOCK_SIZE;
    dir->i_dirdirty = false;
    dir->i_dirty = true;
}

/* Free directory entries */
void
dfs_dirFree(struct inode *dir) {
    struct dirent *dirent = dir->i_dirent, *tmp;

    if (dir->i_shared) {
        dir->i_dirent = NULL;
        return;
    }
    while (dirent != NULL) {
        tmp = dirent;
        dirent = dirent->di_next;
        free(tmp->di_name);
        free(tmp);
    }
    dir->i_dirent = NULL;
}

/* Remove a directory tree */
void
dfs_removeTree(struct fs *fs, struct inode *dir) {
    struct dirent *dirent = dir->i_dirent;

    dir->i_removed = true;
    while (dirent != NULL) {
        dfs_printf("dfs_removeTree: dir %ld nlink %ld removing %s inode %ld dir %d\n", dir->i_stat.st_ino, dir->i_stat.st_nlink, dirent->di_name, dirent->di_ino, S_ISDIR(dirent->di_mode));
        dremove(fs, dir, dirent->di_name, dirent->di_ino,
                S_ISDIR(dirent->di_mode));
        dirent = dir->i_dirent;
    }
}
