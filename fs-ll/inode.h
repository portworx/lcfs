#ifndef _INODE_H_
#define _INODE_H_

#include "includes.h"

/* Initial size of the inode table */
/* XXX Do this dynamically */
#define DFS_ICACHE_SIZE 20000

/* Current file name size limit */
#define DFS_FILENAME_MAX 255

/* Directory entry */
struct dirent {

    /* Inode number */
    ino_t di_ino;

    /* Next entry in the directory */
    struct dirent *di_next;

    /* Name of the file/directory */
    char *di_name;

    /* Size of name */
    int16_t di_size;

    /* File mode */
    mode_t di_mode;
};

/* Inode structure */
struct inode {

    /* Stat information */
    struct stat i_stat;

    /* Lock serializing operations on the inode */
    pthread_rwlock_t i_rwlock;

    /* Open count */
    uint64_t i_ocount;

    /* Parent inode number for singly linked inodes */
    uint64_t i_parent;

    union {

        /* Page list of regular file */
        struct page *i_page;

        /* Directory entries of a directory */
        struct dirent *i_dirent;

        /* Target of a symbolic link */
        char *i_target;
    };

    /* last page */
    struct page *i_lpage;

    /* Set if file is marked for removal */
    bool i_removed;

    /* Set if page list if shared between inodes in a snapshot chain */
    bool i_shared;
};

/* XXX Replace ino_t with fuse_ino_t */
/* XXX Make inode numbers 32 bit */

/* Set up inode handle using inode number and file system id */
static inline uint64_t
dfs_setHandle(struct fs *fs, ino_t ino) {
    if (fs->fs_root == DFS_ROOT_INODE) {
        return ino;
    }
    return (fs->fs_root << 32) | ino;
}

/* Get the file system id from the file handle */
static inline ino_t
dfs_getFsHandle(uint64_t fh) {
    ino_t root;

    if (fh == 1) {
        return DFS_ROOT_INODE;
    }
    root = fh >> 32;
    return root ? root : DFS_ROOT_INODE;
}

/* Get inode number corresponding to the file handle */
static inline ino_t
dfs_getInodeHandle(uint64_t fh) {
    if (fh == 1) {
        return DFS_ROOT_INODE;
    }
    return fh & 0xFFFFFFFF;
}

#endif
