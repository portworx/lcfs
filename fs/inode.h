#ifndef _INODE_H_
#define _INODE_H_

#include "includes.h"

/* Initial size of the inode hash table */
/* XXX This needs to consider available memory */
#define DFS_ICACHE_SIZE 1024

/* Current file name size limit */
#define DFS_FILENAME_MAX 255

/* Inode cache header */
struct icache {

    /* Lock protecting the hash chain */
    pthread_mutex_t ic_lock;

    /* Inode hash chains */
    struct inode *ic_head;
};

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
}  __attribute__((packed));

/* Extended attributes of an inode */
struct xattr {
    /* Name of the attribute */
    char *x_name;

    /* Value associated with the attribute */
    char *x_value;

    /* Size of the attribute */
    size_t x_size;

    /* Next xattr in the list */
    struct xattr *x_next;
} __attribute__((packed));

/* Inode structure */
struct inode {

    /* Disk inode part */
    struct dinode i_dinode;

    /* Location of the inode */
    uint64_t i_block;

    /* Lock serializing operations on the inode */
    pthread_rwlock_t i_rwlock;

    /* Filesystem inode belongs to */
    struct fs *i_fs;

    /* Next entry in the hash list */
    struct inode *i_cnext;

    /* Open count */
    uint64_t i_ocount;

    union {

        /* Page list of regular file */
        struct page *i_page;

        /* Directory entries of a directory */
        struct dirent *i_dirent;

        /* Target of a symbolic link */
        char *i_target;
    };

    /* Size of page array */
    uint64_t i_pcount;

    /* Extended attributes */
    struct xattr *i_xattr;

    /* Size of extended attributes */
    size_t i_xsize;

    /* Set if file is marked for removal */
    bool i_removed;

    /* Set if page list if shared between inodes in a snapshot chain */
    bool i_shared;

    /* Set if pages can be cached in kernel */
    bool i_pcache;

    /* Set if inode is dirty */
    bool i_dirty;

    /* Set if inode blockmap is dirty */
    bool i_bmapdirty;

    /* Set if directory is dirty */
    bool i_dirdirty;

    /* Set if extended attributes are dirty */
    bool i_xattrdirty;
}  __attribute__((packed));

#define i_stat          i_dinode.di_stat
#define i_parent        i_dinode.di_parent
#define i_bmapBlock     i_dinode.di_bmap
#define i_xattrBlock    i_dinode.di_xattr

/* XXX Replace ino_t with fuse_ino_t */
/* XXX Make inode numbers 32 bit */

/* Set up inode handle using inode number and file system id */
static inline uint64_t
dfs_setHandle(uint64_t gindex, ino_t ino) {
    assert(gindex < DFS_FS_MAX);
    return (gindex << 32) | ino;
}

/* Get the file system id from the file handle */
static inline uint64_t
dfs_getFsHandle(uint64_t handle) {
    int gindex = handle >> 32;

    assert(gindex < DFS_FS_MAX);
    return gindex;
}

/* Get inode number corresponding to the file handle */
static inline ino_t
dfs_getInodeHandle(uint64_t handle) {
    if (handle <= DFS_ROOT_INODE) {
        return DFS_ROOT_INODE;
    }
    return handle & 0xFFFFFFFF;
}

/* Mark inode dirty for flushing to disk */
static inline void
dfs_markInodeDirty(struct inode *inode, bool dirty, bool dir, bool bmap,
                   bool xattr) {
    if (dirty) {
        inode->i_dirty = true;
    }
    if (dir) {
        inode->i_dirdirty = true;
    }
    if (bmap) {
        inode->i_bmapdirty = true;
    }
    if (xattr) {
        inode->i_xattrdirty = true;
    }
}

/* Check an inode is dirty or not */
static inline bool
dfs_inodeDirty(struct inode *inode) {
    return inode->i_dirty || inode->i_dirdirty || inode->i_bmapdirty ||
           inode->i_xattrdirty;
}


#endif
