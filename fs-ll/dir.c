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
dfs_dirAdd(struct inode *dir, ino_t ino, mode_t mode, const char *name) {
    struct dirent *dirent = malloc(sizeof(struct dirent));
    int nsize = strlen(name);

    assert(S_ISDIR(dir->i_stat.st_mode));
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
dfs_dirCopy(struct inode *inode, struct inode *dir) {
    struct dirent *dirent = dir->i_dirent;

    assert(S_ISDIR(inode->i_stat.st_mode));
    assert(S_ISDIR(dir->i_stat.st_mode));
    assert(dir->i_stat.st_nlink >= 2);
    while (dirent) {
        dfs_dirAdd(inode, dirent->di_ino, dirent->di_mode, dirent->di_name);
        dirent = dirent->di_next;
    }
    inode->i_stat.st_nlink = dir->i_stat.st_nlink;
}


/* Remove a directory entry */
void
dfs_dirRemove(struct inode *dir, const char *name) {
    struct dirent *dirent = dir->i_dirent;
    struct dirent *pdirent = NULL;
    int len = strlen(name);

    assert(S_ISDIR(dir->i_stat.st_mode));
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

/* Free directory entries */
void
dfs_dirFree(struct inode *dir) {
    struct dirent *dirent = dir->i_dirent, *tmp;

    while (dirent != NULL) {
        tmp = dirent;
        dirent = dirent->di_next;
        free(tmp);
    }
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
