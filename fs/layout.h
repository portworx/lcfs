#ifndef _LAYOUT_H__
#define _LAYOUT_H__

#define DFS_VERSION     1
#define DFS_SUPER_MAGIC 0x5F5F5F5F
#define DFS_SUPER_BLOCK 0
#define DFS_BLOCK_SIZE  4096
#define DFS_ROOT_INODE  2
#define DFS_INVALID_BLOCK   -1
#define DFS_INVALID_INODE   -1
#define DFS_START_BLOCK (DFS_SUPER_BLOCK + 1)
#define DFS_START_INODE DFS_ROOT_INODE

#define DFS_BMAP_MAGIC  0x6452FABC
#define DFS_DIR_MAGIC   0x7FBD853A
#define DFS_XATTR_MAGIC 0xBDEF4389

/* Superblock Flags */
#define DFS_SUPER_DIRTY     0x00000001  // Snapshot is dirty
#define DFS_SUPER_RDWR      0x00000002  // Snapshot is readwrite

/* File system superblock */
struct super {

    /* A magic number */
    uint32_t sb_magic;

    /* Various flags */
    uint32_t sb_flags;

    /* Root inode */
    uint64_t sb_root;

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

    /* Next block available for allocation */
    uint64_t sb_nblock;

    /* Count of inodes in use */
    uint64_t sb_inodes;

    /* Next inode available for allocation */
    uint64_t sb_ninode;

    /* Version of the file system layout */
    uint32_t sb_version;

    /* Padding for filling up a block */
    uint8_t  sb_pad[DFS_BLOCK_SIZE - 100];
} __attribute__((packed));
static_assert(sizeof(struct super) == DFS_BLOCK_SIZE, "superblock size != DFS_BLOCK_SIZE");

/* Disk inode structure */
struct dinode {

    /* Stat structure */
    /* XXX Avoid storing unwanted stat fields */
    struct stat di_stat;

    /* Block tracking block map */
    uint64_t di_bmap;

    /* Block tracking extended attributes */
    uint64_t di_xattr;

    /* Parent inode number of singly linked inodes */
    uint64_t di_parent;
} __attribute__((packed));
static_assert(sizeof(struct dinode) == 168, "dinode size != 168");

#define DFS_IBLOCK_MAX  ((DFS_BLOCK_SIZE / sizeof(uint64_t)) - 2)
/* Inode block table */
struct iblock {
    /* Magic number */
    uint32_t ib_magic;

    /* CRC of the block */
    uint32_t ib_crc;

    /* Next block */
    uint64_t ib_next;

    /* Inode blocks */
    uint64_t ib_blks[DFS_IBLOCK_MAX];
};
static_assert(sizeof(struct iblock) == DFS_BLOCK_SIZE, "iblock size != DFS_BLOCK_SIZE");

/* Bmap block entry */
struct bmap {
    /* Offset */
    uint64_t b_off;

    /* Block number */
    uint64_t b_block;
};
static_assert(sizeof(struct bmap) == 16, "bmap size != 16");

/* Number of bmap entries in a block */
#define DFS_BMAP_BLOCK ((DFS_BLOCK_SIZE / sizeof(struct bmap)) - 1)

/* Bmap block structure */
struct bmapBlock {
    /* Magic number */
    uint32_t bb_magic;

    /* Checksum */
    uint32_t bb_crc;

    /* Next block */
    uint64_t bb_next;

    /* Bmap entries in a block */
    struct bmap bb_bmap[DFS_BMAP_BLOCK];
};
static_assert(sizeof(struct bmapBlock) == DFS_BLOCK_SIZE, "bmapBlock size != DFS_BLOCK_SIZE");

/* Directory entry structure */
struct ddirent {

    /* Inode number */
    uint64_t di_inum;

    /* Type of entry */
    uint8_t di_type;

    /* Length of name */
    uint16_t di_len;

    /* Name of entry */
    char di_name[0];
} __attribute__((packed));
 static_assert(sizeof(struct ddirent) == 11, "ddirent size != 11");

/* Extended attribute entry */
struct dxattr {
    /* Length of name */
    uint16_t dx_nsize;

    /* Length of value */
    uint16_t dx_nvalue;

    /* Name of the attribute */
    char dx_name[0];

    /* Name of the value */
    char dx_value[0];
} __attribute__((packed));
 static_assert(sizeof(struct dxattr) == 4, "dxattr size != 4");

#endif
