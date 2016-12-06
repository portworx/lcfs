#ifndef _INODE_H_
#define _INODE_H_

#include "includes.h"

/* Initial size of the inode hash table */
/* XXX This needs to consider available memory */
#define LC_ICACHE_SIZE_MIN 128
#define LC_ICACHE_SIZE     1024
#define LC_ICACHE_SIZE_MAX 2048

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

/* Data specific for regular files */
struct rdata {

    /* Dirty pages */
    struct dpage *rd_page;

    /* Block map */
    uint64_t *rd_bmap;

    /* Size of bmap array */
    uint64_t rd_bcount;

    /* Size of page array */
    uint64_t rd_pcount;

    /* Count of dirty pages */
    uint64_t rd_dpcount;

    /* Set if inode blockmap is dirty */
    bool rd_bmapdirty;
} __attribute__((packed));

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

/* Optional extended attributes linked from the inode */
struct ixattr {

    /* Extended attributes */
    struct xattr *xd_xattr;

    /* Size of extended attributes */
    size_t xd_xsize;

    /* Extents for bmap or directory blocks */
    struct extent *xd_xattrExtents;

    /* Set if extended attributes are dirty */
    bool xd_xattrdirty;
} __attribute__((packed));

/* Inode structure */
struct inode {

    /* Disk inode part */
    struct dinode i_dinode;

    /* Location of the inode */
    uint64_t i_block;

    /* Open count */
    uint64_t i_ocount;

    /* Filesystem inode belongs to */
    struct fs *i_fs;

    /* Lock serializing operations on the inode */
    pthread_rwlock_t i_rwlock;

    /* Next entry in the hash list */
    struct inode *i_cnext;

    /* Next entry in the dirty list */
    struct inode *i_dnext;

    /* Extents for bmap or directory blocks */
    struct extent *i_bmapDirExtents;

    /* Optional extended attributes */
    struct ixattr *i_xattrData;

    union {

        /* Data specific for regular files */
        struct rdata *i_rdata;

        /* Directory entries of a directory */
        struct dirent *i_dirent;

        /* Target of a symbolic link */
        char *i_target;
    };

    /* Set if file is marked for removal */
    bool i_removed;

    /* Set if page or directory list if shared between inodes in a tree */
    bool i_shared;

    /* Set if inode is dirty */
    bool i_dirty;

    /* Set if directory is dirty */
    bool i_dirdirty;

}  __attribute__((packed));
static_assert(sizeof(struct inode) == 241, "inode size != 241");

#define i_parent        i_dinode.di_parent
#define i_xattrBlock    i_dinode.di_xattr
#define i_private       i_dinode.di_private
#define i_bmapDirBlock  i_dinode.di_bmapdir
#define i_extentBlock   i_dinode.di_bmapdir
#define i_extentLength  i_dinode.di_extentLength

#define i_page          i_rdata->rd_page
#define i_bmap          i_rdata->rd_bmap
#define i_bcount        i_rdata->rd_bcount
#define i_pcount        i_rdata->rd_pcount
#define i_dpcount       i_rdata->rd_dpcount
#define i_bmapdirty     i_rdata->rd_bmapdirty

#define i_xattr         i_xattrData->xd_xattr
#define i_xsize         i_xattrData->xd_xsize
#define i_xattrExtents  i_xattrData->xd_xattrExtents
#define i_xattrdirty    i_xattrData->xd_xattrdirty

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
        assert(S_ISDIR(inode->i_dinode.di_mode));
        inode->i_dirdirty = true;
    }
    if (bmap) {
        assert(S_ISREG(inode->i_dinode.di_mode));
        inode->i_bmapdirty = true;
    }
    if (xattr) {
        inode->i_xattrdirty = true;
    }
}

/* Check an inode is dirty or not */
static inline bool
lc_inodeDirty(struct inode *inode) {
    return inode->i_dirty || inode->i_dirdirty ||
           (S_ISREG(inode->i_dinode.di_mode) && inode->i_bmapdirty) ||
           (inode->i_xattrData && inode->i_xattrdirty);
}

#endif
