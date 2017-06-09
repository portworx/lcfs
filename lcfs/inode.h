#ifndef _INODE_H_
#define _INODE_H_

#include "includes.h"

/* Initial size of the inode hash table */
/* XXX This needs to consider available memory */
#define LC_ICACHE_SIZE_MIN 8
#define LC_ICACHE_SIZE     1024
#define LC_ICACHE_SIZE_MAX 8192

/* Used to size icache from number of inodes in the layer */
#define LC_ICACHE_TARGET   2

/* Current file name size limit */
#define LC_FILENAME_MAX 255

/* Inode cache header */
struct icache {

#ifdef LC_IC_LOCK
    /* Lock protecting the hash chain */
    pthread_mutex_t ic_lock;
#endif

    /* Inode hash chains */
    struct inode *ic_head;

    /* Smallest inode number in the list */
    ino_t ic_lowInode;

    /* Smallest inode number in the list */
    ino_t ic_highInode;
};

/* Minimum directory size before converting to hash table */
#define LC_DIRCACHE_MIN  32

/* Size of the directory hash table */
/* XXX Use a hash size proportional to the size of the directory.
 * Rehash as the directory grows.
 */
#define LC_DIRCACHE_SIZE 512

/* Number of characters included from the name for calculating hash */
#define LC_DIRHASH_LEN   10

/* Bytes shifted in readdir offset for storing hash index */
#define LC_DIRHASH_SHIFT 32ul

/* Portion of the readdir offset storing index in the list */
#define LC_DIRHASH_INDEX 0x00000000FFFFFFFFul

/* Directory entry */
struct dirent {

    /* Inode number */
    uint64_t di_ino:LC_FH_LAYER;

    /* Size of name */
    uint64_t di_size:16;

    /* Next entry in the directory */
    struct dirent *di_next;

    /* Name of the file/directory */
    char *di_name;

    /* Index of this entry in the directory */
    uint32_t di_index;

    /* File mode */
    mode_t di_mode;
}  __attribute__((packed));

/* Data specific for regular files */
struct rdata {

    /* Extent map */
    struct extent *rd_emap;

    /* Next entry in the dirty list */
    struct inode *rd_dnext;

    /* Index of last flusher */
    uint64_t rd_flusher;

    /* First dirty page */
    uint32_t rd_fpage;

    /* Last dirty page */
    uint32_t rd_lpage;

    /* Size of page array */
    uint32_t rd_pcount;

    /* Count of dirty pages */
    uint32_t rd_dpcount;
} __attribute__((packed));
static_assert(sizeof(struct rdata) == 40, "rdata size != 40");

/* Data tracked for hard links */
struct hldata {

    /* Inode number */
    ino_t hl_ino;

    /* Parent directory */
    ino_t hl_parent;

    /* Next in the list */
    struct hldata *hl_next;

    /* Number of links */
    uint32_t hl_nlink;
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

#define LC_INODE_DIRTY          0x0001  /* Inode is dirty */
/* XXX LC_INODE_EMAPDIRTY and LC_INODE_DIRDIRTY may use the same bit. */
#define LC_INODE_EMAPDIRTY      0x0002  /* Dirty pages and Emap */
#define LC_INODE_DIRDIRTY       0x0004  /* Dirty directory */
#define LC_INODE_XATTRDIRTY     0x0008  /* Dirty extended attributes */
#define LC_INODE_REMOVED        0x0010  /* File is removed */
#define LC_INODE_SHARED         0x0020  /* Sharing emap/directory of parent */
#define LC_INODE_TMP            0x0040  /* Created under /tmp */
#define LC_INODE_HASHED         0x0080  /* Hashed directory */
#define LC_INODE_DHASHED        0x0080  /* Dirty pages in a hash table */
#define LC_INODE_NOTRUNC        0x0100  /* Do not truncate this file */
#define LC_INODE_CTRACKED       0x0200  /* Inode in the change list */
#define LC_INODE_MLINKS         0x0400  /* Linked from many directories */
#define LC_INODE_SYMLINK        0x0800  /* Free symbolic link target */
#define LC_INODE_DISK           0x1000  /* Inode flushed to disk */
#define LC_INODE_HIDDEN         0x2000  /* Inode is hidden from child layers */

#ifndef LC_DIFF
/* Fake inode number used to trigger layer commit operation */
#define LC_COMMIT_TRIGGER_INODE     LC_ROOT_INODE
#endif

/* Number of inode pages which can be freed if inodes are re-written */
#define LC_INODE_RELOCATE_PCOUNT    10

/* Inode structure */
struct inode {

    /* Disk inode part */
    struct dinode i_dinode;

    /* Filesystem inode belongs to */
    struct fs *i_fs;

    /* Lock serializing operations on the inode */
    pthread_rwlock_t *i_rwlock;

    /* Next entry in the hash list */
    struct inode *i_cnext;

    /* Extents for emap or directory blocks */
    struct extent *i_emapDirExtents;

    /* Optional extended attributes */
    struct ixattr *i_xattrData;

    union {

        /* Array of Dirty pages */
        struct dpage *i_page;

        /* Hash table for dirty pages */
        struct dhpage **i_hpage;

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
    uint32_t i_flags;
#ifdef __APPLE__
    /* Padding for darwin */
#define DARWIN_INODE_SIZE 6
    char opaque[DARWIN_INODE_SIZE];
#endif
}  __attribute__((packed));
static_assert(sizeof(struct inode) == 160, "inode size != 160");
static_assert((sizeof(struct inode) % sizeof(void *)) == 0,
              "Inode size is not aligned");

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

#define i_xattr         i_xattrData->xd_xattr
#define i_xsize         i_xattrData->xd_xsize
#define i_xattrExtents  i_xattrData->xd_xattrExtents

static inline struct rdata *
lc_inodeGetRegData(struct inode *inode) {
    return (struct rdata *)(((char *)inode) + sizeof(struct inode));
}

/* Return the first extent in the emap list */
static inline struct extent *
lc_inodeGetEmap(struct inode *inode) {
    struct rdata *rdata = lc_inodeGetRegData(inode);

    return rdata->rd_emap;
}

/* Return the address in inode storing emap list */
static inline struct extent **
lc_inodeGetEmapPtr(struct inode *inode) {
    struct rdata *rdata = lc_inodeGetRegData(inode);

    return &rdata->rd_emap;
}

/* Set the inode emap to the specified extent */
static inline void
lc_inodeSetEmap(struct inode *inode, struct extent *extent) {
    struct rdata *rdata = lc_inodeGetRegData(inode);

    rdata->rd_emap = extent;
}

/* Return the size of inode page array */
static inline uint32_t
lc_inodeGetPageCount(struct inode *inode) {
    struct rdata *rdata = lc_inodeGetRegData(inode);

    return rdata->rd_pcount;
}

/* Set the size of inode page array */
static inline void
lc_inodeSetPageCount(struct inode *inode, uint32_t count) {
    struct rdata *rdata = lc_inodeGetRegData(inode);

    rdata->rd_pcount = count;
}

/* Return the count of dirty pages */
static inline uint32_t
lc_inodeGetDirtyPageCount(struct inode *inode) {
    struct rdata *rdata = lc_inodeGetRegData(inode);

    return rdata->rd_dpcount;
}

/* Increment dirty page count */
static inline void
lc_inodeIncrDirtyPageCount(struct inode *inode) {
    struct rdata *rdata = lc_inodeGetRegData(inode);

    rdata->rd_dpcount++;
}

/* Decrement dirty page count */
static inline void
lc_inodeDecrDirtyPageCount(struct inode *inode) {
    struct rdata *rdata = lc_inodeGetRegData(inode);

    assert(rdata->rd_dpcount > 0);
    rdata->rd_dpcount--;
}

/* Return flusher id */
static inline uint64_t
lc_inodeGetFlusher(struct inode *inode) {
    struct rdata *rdata = lc_inodeGetRegData(inode);

    return rdata->rd_flusher;
}

/* Set flusher id */
static inline void
lc_inodeSetFlusher(struct inode *inode, uint64_t id) {
    struct rdata *rdata = lc_inodeGetRegData(inode);

    rdata->rd_flusher = id;
}

/* Retrun next dirty inode after the inode */
static inline struct inode *
lc_inodeGetDirtyNext(struct inode *inode) {
    struct rdata *rdata = lc_inodeGetRegData(inode);

    return rdata->rd_dnext;
}

/* Set next dirty inode after the inode */
static inline void
lc_inodeSetDirtyNext(struct inode *inode, struct inode *next) {
    struct rdata *rdata = lc_inodeGetRegData(inode);

    rdata->rd_dnext = next;
}

/* Return first dirty page */
static inline uint32_t
lc_inodeGetFirstPage(struct inode *inode) {
    struct rdata *rdata = lc_inodeGetRegData(inode);

    return rdata->rd_fpage;
}

/* Set first dirty page */
static inline void
lc_inodeSetFirstPage(struct inode *inode, uint64_t page) {
    struct rdata *rdata = lc_inodeGetRegData(inode);

    rdata->rd_fpage = page;
}

/* Return last dirty page */
static inline uint32_t
lc_inodeGetLastPage(struct inode *inode) {
    struct rdata *rdata = lc_inodeGetRegData(inode);

    return rdata->rd_lpage;
}

/* Set last dirty page */
static inline void
lc_inodeSetLastPage(struct inode *inode, uint64_t page) {
    struct rdata *rdata = lc_inodeGetRegData(inode);

    rdata->rd_lpage = page;
}

/* Mark inode dirty for flushing to disk */
static inline void
lc_markInodeDirty(struct inode *inode, uint32_t flags) {
    assert(!(flags & LC_INODE_DIRDIRTY) || S_ISDIR(inode->i_dinode.di_mode));
    assert(!(flags & LC_INODE_EMAPDIRTY) || S_ISREG(inode->i_dinode.di_mode));

    /* Reset notrunc flag when data modified in a layer */
    if (flags & LC_INODE_EMAPDIRTY) {
        inode->i_flags &= ~LC_INODE_NOTRUNC;
    }
    inode->i_flags |= flags | LC_INODE_DIRTY;
    lc_markInodesDirty(inode->i_fs);
}

/* Check an inode is dirty or not */
static inline bool
lc_inodeDirty(struct inode *inode) {
    return (inode->i_flags & (LC_INODE_DIRTY | LC_INODE_DIRDIRTY |
                              LC_INODE_EMAPDIRTY | LC_INODE_XATTRDIRTY));
}

/* Find size of icache size based on number of inodes */
static inline size_t
lc_icache_size(struct fs *fs) {
    struct super *super = fs->fs_super;
    uint64_t icount, icsize = 1;

    if (super->sb_flags & LC_SUPER_INIT) {
        return LC_ICACHE_SIZE_MIN;
    }
    if (super->sb_flags & LC_SUPER_RDWR) {
        return LC_ICACHE_SIZE;
    }

    /* Find next power of two */
    icount = super->sb_icount / LC_ICACHE_TARGET;
    if (icount) {
        icount--;
        while (icount >>= 1) {
            icsize <<= 1;
        }
    }
    if (icsize <= LC_ICACHE_SIZE_MIN) {
        return LC_ICACHE_SIZE_MIN;
    }
    if (icsize >= LC_ICACHE_SIZE_MAX) {
        return LC_ICACHE_SIZE_MAX;
    }
    return icsize;
}

/* Invalidate pages of an inode in kernel page cache */
static inline void
lc_invalInodePages(struct gfs *gfs, ino_t ino) {
    if (lc_getGlobalFs(gfs)->fs_mcount) {
        fuse_lowlevel_notify_inval_inode(
#ifdef FUSE3
                                     gfs->gfs_se[LC_LAYER_MOUNT],
#else
                                     gfs->gfs_ch[LC_LAYER_MOUNT],
#endif
                                     ino, 0, -1);
    }
}

#endif
