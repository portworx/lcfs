#ifndef _INODE_H_
#define _INODE_H_

#include "includes.h"

/* Initial size of the inode hash table */
/* XXX This needs to consider available memory */
#define LC_ICACHE_SIZE 1024

/* Current file name size limit */
#define LC_FILENAME_MAX 255

/* Attempt to cluster these many inodes together */
#define LC_INODE_CLUSTER_SIZE   LC_CLUSTER_SIZE

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

    /* Index of this entry in the directory */
    uint64_t di_index;

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

    /* Next entry in the dirty list */
    struct inode *i_dnext;

    /* Open count */
    uint64_t i_ocount;

    union {

        /* Dirty pages */
        struct dpage *i_page;

        /* Directory entries of a directory */
        struct dirent *i_dirent;

        /* Target of a symbolic link */
        char *i_target;
    };

    /* Block map */
    uint64_t *i_bmap;

    /* Size of bmap array */
    uint64_t i_bcount;

    /* Size of page array */
    uint64_t i_pcount;

    /* Count of dirty pages */
    uint64_t i_dpcount;

    /* Extended attributes */
    struct xattr *i_xattr;

    /* Size of extended attributes */
    size_t i_xsize;

    /* Extents for bmap or directory blocks */
    struct extent *i_bmapDirExtents;

    /* Extents for bmap or directory blocks */
    struct extent *i_xattrExtents;

    /* Set if file is marked for removal */
    bool i_removed;

    /* Set if page list if shared between inodes in a snapshot chain */
    bool i_shared;

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
#define i_bmapDirBlock  i_dinode.di_bmapdir
#define i_xattrBlock    i_dinode.di_xattr
#define i_private       i_dinode.di_private
#define i_extentBlock   i_dinode.di_bmapdir
#define i_extentLength  i_dinode.di_extentLength

/* XXX Replace ino_t with fuse_ino_t */
/* XXX Make inode numbers 32 bit */

/* Mark inode dirty for flushing to disk */
static inline void
lc_markInodeDirty(struct inode *inode, bool dirty, bool dir, bool bmap,
                   bool xattr) {
    if (dirty) {
        inode->i_dirty = true;
    }
    if (dir) {
        assert(S_ISDIR(inode->i_stat.st_mode));
        inode->i_dirdirty = true;
    }
    if (bmap) {
        assert(S_ISREG(inode->i_stat.st_mode));
        inode->i_bmapdirty = true;
    }
    if (xattr) {
        inode->i_xattrdirty = true;
    }
}

/* Check an inode is dirty or not */
static inline bool
lc_inodeDirty(struct inode *inode) {
    return inode->i_dirty || inode->i_dirdirty || inode->i_bmapdirty ||
           inode->i_xattrdirty;
}


#endif
