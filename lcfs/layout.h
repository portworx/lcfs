#ifndef _LAYOUT_H__
#define _LAYOUT_H__

#define LC_VERSION     1
#define LC_SUPER_MAGIC 0x5F5F5F5F
#define LC_SUPER_BLOCK 0
#define LC_BLOCK_SIZE  4096
#define LC_ROOT_INODE  2
#define LC_INVALID_BLOCK   -1
#define LC_INVALID_INODE   -1
#define LC_START_BLOCK (LC_SUPER_BLOCK + 1)
#define LC_START_INODE LC_ROOT_INODE
#define LC_MIN_BLOCKS       10000ul
#define LC_LAYER_MIN_BLOCKS 10000ul

#define LC_FH_LAYER     48ul
#define LC_FH_INODE     0x0000FFFFFFFFFFFFul

#define LC_BMAP_MAGIC  0x6452FABC
#define LC_DIR_MAGIC   0x7FBD853A
#define LC_XATTR_MAGIC 0xBDEF4389

/* Superblock Flags */
#define LC_SUPER_DIRTY     0x00000001  // Layer is dirty
#define LC_SUPER_RDWR      0x00000002  // Layer is readwrite
#define LC_SUPER_MOUNTED   0x00000004  // Layer is mounted

/* File system superblock */
struct super {

    /* A magic number */
    uint32_t sb_magic;

    /* Various flags */
    uint32_t sb_flags;

    /* Root inode */
    uint64_t sb_root;

    /* Allocated/free extent list */
    uint64_t sb_extentBlock;

    /* Inode start block */
    uint64_t sb_inodeBlock;

    /* Next snapshot in sibling chain */
    uint64_t sb_nextSnap;

    /* First child snapshot */
    uint64_t sb_childSnap;

    /* CRC of this block */
    uint32_t sb_crc;

    /* Index of file system */
    uint32_t sb_index;

    /* Following fields are maintained only for the global file system */

    /* Number of times file system mounted */
    uint64_t sb_mounts;

    /* Total number of file system blocks */
    uint64_t sb_tblocks;

    /* Count of blocks in use */
    uint64_t sb_blocks;

    /* Count of inodes in use */
    uint64_t sb_inodes;

    /* Next inode available for allocation */
    uint64_t sb_ninode;

    /* Version of the file system layout */
    uint32_t sb_version;

    /* Padding for filling up a block */
    uint8_t  sb_pad[LC_BLOCK_SIZE - 100];
} __attribute__((packed));
static_assert(sizeof(struct super) == LC_BLOCK_SIZE, "superblock size != LC_BLOCK_SIZE");

/* Extent entry */
struct dextent {
    /* Starting block */
    uint64_t de_start;

    /* Count of blocks */
    uint64_t de_count;
};
static_assert(sizeof(struct dextent) == 16, "dextent size != 16");

/* Number of extent entries in a block */
#define LC_EXTENT_BLOCK ((LC_BLOCK_SIZE / sizeof(struct dextent)) - 1)

/* Extent block structure */
struct dextentBlock {
    /* Magic number */
    uint32_t de_magic;

    /* Checksum */
    uint32_t de_crc;

    /* Next block */
    uint64_t de_next;

    /* Extent entries in a block */
    struct dextent de_extents[LC_EXTENT_BLOCK];
};
static_assert(sizeof(struct dextentBlock) == LC_BLOCK_SIZE, "dextent size != LC_BLOCK_SIZE");

/* Disk inode structure */
struct dinode {

    /* Stat structure */
    /* XXX Avoid storing unwanted stat fields */
    struct stat di_stat;

    /* Starting block for bmap or directory */
    uint64_t di_bmapdir;

    /* Length of extent if directly pointed by di_bmapdir */
    uint64_t di_extentLength;

    /* Block tracking extended attributes */
    uint64_t di_xattr;

    /* Parent inode number of singly linked inodes */
    uint64_t di_parent;

    /* Set if blocks are newly allocated and not inherited */
    uint8_t di_private;
} __attribute__((packed));
static_assert(sizeof(struct dinode) == 177, "dinode size != 177");

#define LC_IBLOCK_MAX  ((LC_BLOCK_SIZE / sizeof(uint64_t)) - 2)
/* Inode block table */
struct iblock {
    /* Magic number */
    uint32_t ib_magic;

    /* CRC of the block */
    uint32_t ib_crc;

    /* Next block */
    uint64_t ib_next;

    /* Inode blocks */
    uint64_t ib_blks[LC_IBLOCK_MAX];
};
static_assert(sizeof(struct iblock) == LC_BLOCK_SIZE, "iblock size != LC_BLOCK_SIZE");

/* Bmap block entry */
struct bmap {
    /* Offset */
    uint64_t b_off;

    /* Block number */
    uint64_t b_block;
};
static_assert(sizeof(struct bmap) == 16, "bmap size != 16");

/* Number of bmap entries in a block */
#define LC_BMAP_BLOCK ((LC_BLOCK_SIZE / sizeof(struct bmap)) - 1)

/* Bmap block structure */
struct bmapBlock {
    /* Magic number */
    uint32_t bb_magic;

    /* Checksum */
    uint32_t bb_crc;

    /* Next block */
    uint64_t bb_next;

    /* Bmap entries in a block */
    struct bmap bb_bmap[LC_BMAP_BLOCK];
};
static_assert(sizeof(struct bmapBlock) == LC_BLOCK_SIZE, "bmapBlock size != LC_BLOCK_SIZE");

/* Directory entry structure */
struct ddirent {

    /* Inode number */
    uint64_t di_inum;

    /* Type of entry */
    uint16_t di_type;

    /* Length of name */
    uint16_t di_len;

    /* Name of entry */
    char di_name[0];
} __attribute__((packed));
#define LC_MIN_DIRENT_SIZE (sizeof(uint64_t) + (2 * sizeof(uint16_t)))
static_assert(sizeof(struct ddirent) == LC_MIN_DIRENT_SIZE, "ddirent size != 12");

/* Directory block */
struct dblock {
    /* Magic number */
    uint32_t db_magic;

    /* CRC */
    uint32_t db_crc;

    /* Next block */
    uint64_t db_next;

    /* Directory entries */
    struct ddirent db_dirent[0];
} __attribute__((packed));
static_assert(sizeof(struct dblock) == 16, "dblock size != 16");

/* Extended attribute entry */
struct dxattr {
    /* Length of name */
    uint16_t dx_nsize;

    /* Length of value */
    uint16_t dx_nvalue;

    /* Name of the attribute and optional value */
    char dx_nameValue[0];
} __attribute__((packed));
static_assert(sizeof(struct dxattr) == 4, "dxattr size != 4");

/* Extended attribute block */
struct xblock {
    /* Magic number */
    uint32_t xb_magic;

    /* CRC */
    uint32_t xb_crc;

    /* Next block */
    uint64_t xb_next;

    /* Attributes */
    struct dxattr xb_attr[0];
} __attribute__((packed));
static_assert(sizeof(struct xblock) == 16, "xblock size != 16");

#endif
