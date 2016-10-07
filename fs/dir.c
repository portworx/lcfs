#include "includes.h"

/* Lookup the specified name in the directory and return correponding inode
 * number if found.
 */
ino_t
dfs_dirLookup(struct fs *fs, ino_t ino, const char *name) {
    struct dirent *dirent;
    struct inode *dir;
    int len = strlen(name);
    ino_t dino;

    dir = dfs_getInode(fs, ino, false, false);
    if (dir == NULL) {
        return DFS_INVALID_INODE;
    }
    assert((dir->i_stat.st_mode & S_IFMT) == S_IFDIR);
    dirent = dir->i_dirent;
    while (dirent != NULL) {
        if ((len == dirent->di_size) &&
            (strcmp(name, dirent->di_name) == 0)) {
            dino = dirent->di_ino;
            dfs_inodeUnlock(dir);
            return dino;
        }
        dirent = dirent->di_next;
    }
    dfs_inodeUnlock(dir);
    return DFS_INVALID_INODE;
}

/* Find the inode number and optionally the file system and parent directory,
 * corresponding to the specified path
 */
ino_t
dfs_lookup(const char *path, struct gfs *gfs, struct fs **fsp,
           ino_t *dir, char *fname) {
    struct fs *fs = dfs_getfs(gfs, DFS_ROOT_INODE);
    char name[DFS_FILENAME_MAX];
    ino_t ino;
    int i, j;

    if (dir) {
        *dir = DFS_ROOT_INODE;
    }
    if (fsp) {
        *fsp = fs;
    }
    if (fname) {
        fname[0] = 0;
    }
    if ((path == NULL) || (path[0] != '/')) {
        return DFS_INVALID_INODE;
    }
    if (strcmp(path, "/") == 0) {
        return DFS_ROOT_INODE;
    }
    i = j = 0;
    ino = DFS_ROOT_INODE;
    while (path[i] != 0) {

        /* Lookup the path component identified */
        if (path[i] == '/') {
            if (j != 0) {
                name[j] = 0;
                ino = dfs_dirLookup(fs, ino, name);
                if (ino == DFS_INVALID_INODE) {
                    return ino;
                }

                /* Check if this component is in a different file system */
                fs = dfs_checkfs(fs, ino);
                if (fsp) {
                    *fsp = fs;
                }
                if (dir) {
                    *dir = ino;
                }
            }
            j = 0;
            i++;
            continue;
        }
        name[j] = path[i];
        j++;
        i++;
    }

    /* Lookup the last component if the path did not end with '/' */
    if (j > 0) {
        name[j] = 0;
        ino = dfs_dirLookup(fs, ino, name);
        if (fsp && (ino != DFS_INVALID_INODE)) {
            *fsp = dfs_checkfs(fs, ino);
        }
        if (fname) {
            memcpy(fname, name, j + 1);
        }
        return ino;
    }
    return DFS_INVALID_INODE;
}

/* Add a new directory entry to the given directory */
void
dfs_dirAdd(struct inode *dir, ino_t ino, mode_t mode, char *name) {
    struct dirent *dirent = malloc(sizeof(struct dirent));
    int nsize = strlen(name);

    assert((dir->i_stat.st_mode & S_IFMT) == S_IFDIR);
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

    assert((inode->i_stat.st_mode & S_IFMT) == S_IFDIR);
    assert((dir->i_stat.st_mode & S_IFMT) == S_IFDIR);
    while (dirent) {
        dfs_dirAdd(inode, dirent->di_ino, dirent->di_mode, dirent->di_name);
        dirent = dirent->di_next;
    }
}


/* Remove a directory entry */
void
dfs_dirRemove(struct inode *dir, char *name) {
    struct dirent *dirent = dir->i_dirent;
    struct dirent *pdirent = NULL;
    int len = strlen(name);

    assert((dir->i_stat.st_mode & S_IFMT) == S_IFDIR);
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
            break;
        }
        pdirent = dirent;
        dirent = dirent->di_next;
    }
}

/* Rename a directory entry with a new name */
void
dfs_dirRename(struct inode *dir, ino_t ino, char *name) {
    struct dirent *dirent = dir->i_dirent;
    int len = strlen(name);

    assert((dir->i_stat.st_mode & S_IFMT) == S_IFDIR);
    while (dirent != NULL) {
        if (dirent->di_ino == ino) {

            /* Existing name can be used if size is not growing */
            if (len > dirent->di_size) {
                free(dirent->di_name);
                dirent->di_name = malloc(len + 1);
            }
            memcpy(dirent->di_name, name, len);
            dirent->di_name[len] = 0;
            dirent->di_size = len;
            break;
        }
        dirent = dirent->di_next;
    }
}

