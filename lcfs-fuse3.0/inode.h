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

#define LC_DIRCACHE_MIN  32
#define LC_DIRCACHE_SIZE 512
#define LC_DIRHASH_LEN   10
#define LC_DIRHASH_SHIFT 32ul
#define LC_DIRHASH_INDEX 0x00000000FFFFFFFFul

/* Directory entry */
struct dirent {

    /* Inode number */
    ino_t di_ino;

    /* Next entry in the directory */
    struct dirent *di_next;

    /* Name of the file/directory */
    char *di_name;

    /* Index of this entry in the directory */
    uint32_t di_index;

    /* Size of name */
    int16_t di_size;

    /* File mode */
    mode_t di_mode;
}  __attribute__((packed));

/* Data specific for regular files */
struct rdata {

    union {
        /* Array of Dirty pages */
        struct dpage *rd_page;

        /* Hash table for dirty pages */
        struct dhpage **rd_hpage;
    };

    /* Extent map */
    struct extent *rd_emap;

    /* Size of page array */
    uint32_t rd_pcount;

    /* Count of dirty pages */
    uint32_t rd_dpcount;
} __attribute__((packed));

/* Extended attributes of an inode */
struct xattr {
    /* Name of the attribute */
    char *x_name;

    /* Value associated with the attribute */
    char *x_value;

    /* Next xattr in the list */
    struct xattr *x_next;

    /* Size of the attribute */
    uint32_t x_size;
} __attribute__((packed));

/* Optional extended attributes linked from the inode */
struct ixattr {

    /* Extended attributes */
    struct xattr *xd_xattr;

    /* Extents for emap or directory blocks */
    struct extent *xd_xattrExtents;

    /* Size of extended attributes */
    uint32_t xd_xsize;
} __attribute__((packed));

#define LC_INODE_DIRTY          0x01
#define LC_INODE_EMAPDIRTY      0x02
#define LC_INODE_DIRDIRTY       0x04
#define LC_INODE_XATTRDIRTY     0x08
#define LC_INODE_REMOVED        0x10
#define LC_INODE_SHARED         0x20
#define LC_INODE_TMP            0x40
#define LC_INODE_HASHED         0x80
#define LC_INODE_DHASHED        0x80

/* Inode structure */
struct inode {

    /* Disk inode part */
    struct dinode i_dinode;

    /* Location of the inode */
    uint64_t i_block;

    /* Filesystem inode belongs to */
    struct fs *i_fs;

    /* Lock serializing operations on the inode */
    pthread_rwlock_t i_rwlock;

    /* Next entry in the hash list */
    struct inode *i_cnext;

    /* Next entry in the dirty list */
    struct inode *i_dnext;

    /* Extents for emap or directory blocks */
    struct extent *i_emapDirExtents;

    /* Optional extended attributes */
    struct ixattr *i_xattrData;

    union {

        /* Data specific for regular files */
        struct rdata *i_rdata;

        /* Directory entries of a directory */
        struct dirent *i_dirent;

        /* Directory hash table */
        struct dirent **i_hdirent;

        /* Target of a symbolic link */
        char *i_target;
    };

    /* Open count */
    uint32_t i_ocount;

    /* Various flags */
    uint8_t i_flags;
}  __attribute__((packed));
static_assert(sizeof(struct inode) == 234, "inode size != 234");

#define i_ino           i_dinode.di_ino
#define i_mode          i_dinode.di_mode
#define i_size          i_dinode.di_size
#define i_nlink         i_dinode.di_nlink
#define i_parent        i_dinode.di_parent
#define i_xattrBlock    i_dinode.di_xattr
#define i_private       i_dinode.di_private
#define i_emapDirBlock  i_dinode.di_emapdir
#define i_extentBlock   i_dinode.di_emapdir
#define i_extentLength  i_dinode.di_extentLength

#define i_page          i_rdata->rd_page
#define i_hpage         i_rdata->rd_hpage
#define i_emap          i_rdata->rd_emap
#define i_pcount        i_rdata->rd_pcount
#define i_dpcount       i_rdata->rd_dpcount

#define i_xattr         i_xattrData->xd_xattr
#define i_xsize         i_xattrData->xd_xsize
#define i_xattrExtents  i_xattrData->xd_xattrExtents

/* XXX Replace ino_t with fuse_ino_t */
/* XXX Make inode numbers 32 bit */

/* Mark inode dirty for flushing to disk */
static inline void
lc_markInodeDirty(struct inode *inode, bool dirty, bool dir, bool emap,
                  bool xattr) {
    if (dirty) {
        inode->i_flags |= LC_INODE_DIRTY;
    }
    if (dir) {
        assert(S_ISDIR(inode->i_dinode.di_mode));
        inode->i_flags |= LC_INODE_DIRDIRTY;
    } else if (emap) {
        assert(S_ISREG(inode->i_dinode.di_mode));
        inode->i_flags |= LC_INODE_EMAPDIRTY;
    }
    if (xattr) {
        inode->i_flags |= LC_INODE_XATTRDIRTY;
    }
}

/* Check an inode is dirty or not */
static inline bool
lc_inodeDirty(struct inode *inode) {
    return (inode->i_flags & (LC_INODE_DIRTY | LC_INODE_DIRDIRTY |
                              LC_INODE_EMAPDIRTY | LC_INODE_XATTRDIRTY));
}

#endif
