#ifndef _EXTENT_H_
#define _EXTENT_H_

#include "includes.h"

/* Type of extents */
enum lc_extentType {

    /* Extent tracking space */
    LC_EXTENT_SPACE = 0,

    /* Extent tracking inode extent map */
    LC_EXTENT_EMAP = 1,
} __attribute__((packed));

/* Number of bits used to represent block in an emap extent */
#define LC_EXTENT_EMAP_CSIZE    16
#define LC_EXTENT_EMAP_MAX ((1 << LC_EXTENT_EMAP_CSIZE) - 1)

/* Representing an extent on disk */
struct extent {

    /* Type of the extent */
    uint64_t ex_type:1;

    /* Start block */
    uint64_t ex_start:63;

    union {
        /* Count of blocks */
        uint64_t ex_count;

        struct {

            /* Count of blocks */
            uint64_t ex_bcount:LC_EXTENT_EMAP_CSIZE;

            /* Start block number */
            uint64_t ex_block:48;
        };
    };

    /* Next extent on the device */
    struct extent *ex_next;
} __attribute__((packed));

/* Return start of the extent */
static inline uint64_t
lc_getExtentStart(struct extent *extent) {
    return extent->ex_start;
}

/* Return block of the extent */
static inline uint64_t
lc_getExtentBlock(struct extent *extent) {
    return (extent->ex_type == LC_EXTENT_EMAP) ? extent->ex_block : 0;
}

/* Return count of the extent */
static inline uint64_t
lc_getExtentCount(struct extent *extent) {
    return (extent->ex_type == LC_EXTENT_SPACE) ?
           extent->ex_count : extent->ex_bcount;
}

/* Validate an extent */
static inline void
lc_validateExtent(struct gfs *gfs, struct extent *extent) {
    assert((extent->ex_type == LC_EXTENT_SPACE) || (extent->ex_block > 0));
    assert(lc_getExtentCount(extent) > 0);
    assert((gfs == NULL) ||
           ((((extent->ex_type == LC_EXTENT_SPACE) ?
              lc_getExtentStart(extent) : lc_getExtentBlock(extent)) +
             lc_getExtentCount(extent)) <= gfs->gfs_super->sb_tblocks));
}

/* Set start of the extent */
static inline void
lc_setExtentStart(struct extent *extent, uint64_t start) {
    extent->ex_start = start;
}

/* Set block of the extent */
static inline void
lc_setExtentBlock(struct extent *extent, uint64_t block) {
    if (extent->ex_type == LC_EXTENT_EMAP) {
        extent->ex_block = block;
    }
}

/* Set count of the extent */
static inline void
lc_setExtentCount(struct extent *extent, uint64_t count) {
    if (extent->ex_type == LC_EXTENT_SPACE) {
        extent->ex_count = count;
    } else {
        assert(count <= LC_EXTENT_EMAP_MAX);
        extent->ex_bcount = count;
    }
}

/* Initialize an extent */
static inline void
lc_initExtent(struct gfs *gfs, struct extent *extent, enum lc_extentType type,
              uint64_t start, uint64_t block, uint64_t count,
              struct extent *next) {
    assert(count > 0);
    extent->ex_type = type;
    lc_setExtentStart(extent, start);
    lc_setExtentBlock(extent, block);
    lc_setExtentCount(extent, count);
    lc_validateExtent(gfs, extent);
    extent->ex_next = next;
}

/* Increment start of an extent */
static inline void
lc_incrExtentStart(struct gfs *gfs, struct extent *extent, uint64_t count) {
    extent->ex_start += count;
    if (extent->ex_type == LC_EXTENT_EMAP) {
        extent->ex_block += count;
    }
    lc_validateExtent(gfs, extent);
}

/* Decrement start of an extent */
static inline void
lc_decrExtentStart(struct gfs *gfs, struct extent *extent, uint64_t count) {
    extent->ex_start -= count;
    if (extent->ex_type == LC_EXTENT_EMAP) {
        extent->ex_block -= count;
    }
    lc_validateExtent(gfs, extent);
}

/* Increment count of an extent */
static inline void
lc_incrExtentCount(struct gfs *gfs, struct extent *extent, uint64_t count) {
    if (extent->ex_type == LC_EXTENT_SPACE) {
        extent->ex_count += count;
    } else {
        assert((extent->ex_bcount + count) <= LC_EXTENT_EMAP_MAX);
        extent->ex_bcount += count;
    }
    lc_validateExtent(gfs, extent);
}

/* Decrement count of an extent */
static inline bool
lc_decrExtentCount(struct gfs *gfs, struct extent *extent, uint64_t count) {
    uint64_t ecount = (extent->ex_type == LC_EXTENT_SPACE) ?
                      extent->ex_count : extent->ex_bcount;

    if (ecount == count) {
        return true;
    }
    assert(ecount > count);
    if (extent->ex_type == LC_EXTENT_SPACE) {
        extent->ex_count -= count;
    } else {
        extent->ex_bcount -= count;
    }
    lc_validateExtent(gfs, extent);
    return false;
}

/* Check if two extents are adjacent and can be combined */
static inline bool
lc_extentAdjacent(uint64_t estart, uint64_t eblock, uint64_t count,
                  uint64_t nstart, uint64_t nblock, uint64_t ncount) {
    if (eblock) {
        if ((ncount + count) > LC_EXTENT_EMAP_MAX) {
            return false;
        }
        return (((estart + count) == nstart) && ((eblock + count) == nblock));
    }
    assert(nblock == 0);
    return ((estart + count) == nstart);
}

/* Flags used to manage extent list operations */
#define LC_EXTENT_EFREE 0x1  /* Free extents */
#define LC_EXTENT_FLUSH 0x2  /* Flush extent list to disk */
#define LC_EXTENT_LAYER 0x4  /* Keep the extents in layer pool */
#define LC_EXTENT_KEEP  0x8  /* Keep the extents after flush */

#endif
