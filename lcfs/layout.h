#ifndef _LAYOUT_H__
#define _LAYOUT_H__

/* Current layout version */
#define LC_VERSION     2

/* Magic number stored in superblock */
#define LC_SUPER_MAGIC 0x5F5F5F5F

/* Location of global super block */
#define LC_SUPER_BLOCK 0

/* Block size - unit of space allocation */
#define LC_BLOCK_SIZE  4096

/* Root inode number */
#define LC_ROOT_INODE  2

/* Invalid block */
#define LC_INVALID_BLOCK    0x0000FFFFFFFFFFFFul

/* Invalid inode */
#define LC_INVALID_INODE   -1

/* Block at which allocations start */
#define LC_START_BLOCK (LC_SUPER_BLOCK + 1)

/* Starting inode number for allocation */
#define LC_START_INODE LC_ROOT_INODE

/* Maximum number of blocks supported */
#define LC_MAX_BLOCKS       (LC_INVALID_BLOCK - 1)

/* Minimum size of a device for creating a file system */
#define LC_MIN_BLOCKS       10000ul

/* Minimum free space for allowing creation of new layers */
#define LC_LAYER_MIN_BLOCKS 10000ul

/* Number of bits shifted in the file handle to extract the layer index */
#define LC_FH_LAYER     48ul

/* Mask for extracting inode numbder from file handle */
#define LC_FH_INODE     0x0000FFFFFFFFFFFFul

/* Magic number stored in inode blocks */
#define LC_INODE_MAGIC 0x104BAFE8

/* Magic number stored in extent blocks */
#define LC_EXTENT_MAGIC 0xCBA35732

/* Magic number stored in emap blocks */
#define LC_EMAP_MAGIC  0x6452FABC

/* Magic number stored in directory blocks */
#define LC_DIR_MAGIC   0x7FBD853A

/* Magic number stored in extended attribute blocks */
#define LC_XATTR_MAGIC 0xBDEF4389

/* Superblock Flags */
#define LC_SUPER_DIRTY     0x00000001  /* Layer is dirty */
#define LC_SUPER_RDWR      0x00000002  /* Layer is readwrite */
#define LC_SUPER_ICHECK    0x00000004  /* Layer may have duplicate inodes */
#define LC_SUPER_INIT      0x00000008  /* Init layer */
#define LC_SUPER_ZOMBIE    0x00000010  /* Removed layer */
#define LC_SUPER_FSTATS    0x00000020  /* Tracking count of file types */

/* Directory name in which layers are created */
#define LC_LAYER_ROOT_DIR   "lcfs"

/* Directory in which temporary data placed */
#define LC_LAYER_TMP_DIR    "tmp"

/* local-kv.db file in root layer */
#define LC_LAYER_LOCAL_KV_DB "local-kv.db"

/* extension for json files */
#define LC_JSON_EXTN ".json"
#define LC_JSON_LENGTH 5

/* File types for counting each type created in a layer */
enum lc_ftypes {
    LC_FTYPE_REGULAR,           /* Regular file */
    LC_FTYPE_DIRECTORY,         /* Directory */
    LC_FTYPE_SYMBOLIC_LINK,     /* Symbolic link */
    LC_FTYPE_OTHER,             /* Other file types */
    LC_FTYPE_MAX,
};

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

    /* Number of blocks used for Allocated/free extent list */
    uint64_t sb_extentCount;

    /* Inode start block */
    uint64_t sb_inodeBlock;

    /* Next layer in sibling chain */
    uint64_t sb_nextLayer;

    /* First child layer */
    uint64_t sb_childLayer;

    /* Largest inode number in a layer */
    uint64_t sb_lastInode;

    /* Set on promoted layers after commit */
    uint64_t sb_zombie;

    /* Count of inodes in use */
    uint64_t sb_icount;

    /* Last accessed time in seconds since Epoch */
    time_t sb_atime;

    /* Creation time in seconds since Epoch */
    time_t sb_ctime;

    /* Commit time in seconds since Epoch */
    time_t sb_commitTime;

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

    /* Unmount time in seconds since Epoch */
    time_t sb_unmountTime;

    /* Count of file types */
    uint64_t sb_ftypes[LC_FTYPE_MAX];

    /* Number of times committed */
    uint32_t sb_ncommitted;

    /* Version of the file system layout */
    uint32_t sb_version;

    /* Padding for filling up a block */
    uint8_t  sb_pad[LC_BLOCK_SIZE - 200];
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

    /* Inode number */
    ino_t di_ino;

    /* File mode and permissions */
    mode_t di_mode;

    /* Number of links (hardlinks or subdirectories) */
    uint32_t di_nlink;

    /* User id */
    uid_t di_uid;

    /* Group id */
    gid_t di_gid;

    /* Parent inode number of singly linked inodes */
    uint64_t di_parent:63;

    /* Set if blocks are newly allocated and not inherited */
    uint64_t di_private:1;

    /* Device id */
    dev_t di_rdev;

    /* Size of the file */
    off_t di_size;

    /* Count of blocks */
    uint32_t di_blocks;

    /* Length of extent if directly pointed by di_emapdir */
    uint32_t di_extentLength;

    /* modification time */
    struct timespec di_mtime;

    /* change time */
    struct timespec di_ctime;

    /* Starting block for emap or directory */
    uint64_t di_emapdir;

    /* Block tracking extended attributes */
    uint64_t di_xattr;
} __attribute__((packed));
#ifdef __APPLE__
static_assert(sizeof(struct dinode) == 98, "dinode size != 98");
#else
static_assert(sizeof(struct dinode) == 104, "dinode size != 104");
#endif

/* Size of disk inode.  Last 4 bytes in the inode block is used for storing
 * checksum of the whole inode block.
 */
/* XXX This is wasting around 768 bytes in a block */
#define LC_DINODE_SIZE      128
static_assert(sizeof(struct dinode) <= (LC_DINODE_SIZE - sizeof(uint32_t)),
              "dinode size > (LC_DINODE_SIZE - sizeof(uint32_t)");

/* Number of inodes that can be stored in a block */
#define LC_INODE_BLOCK_MAX  (LC_BLOCK_SIZE / LC_DINODE_SIZE)

/* Index of the inode in an inode block */
#define LC_DINODE_INDEX     48ul

/* Mask for finding the inode block number from i_block */
#define LC_DINODE_BLOCK     0x0000FFFFFFFFFFFFul

/* Inode extent */
struct iextent {

    /* Extent start block */
    uint64_t ie_start;

    /* Number of blocks in the extent */
    uint32_t ie_count;
} __attribute__((packed));

/* Number of inode block numbers that can be stored in a block */
#define LC_IBLOCK_MAX  ((LC_BLOCK_SIZE - \
                         ((2 * sizeof(uint32_t)) + sizeof(uint64_t))) \
                        / sizeof(struct iextent))

/* Inode block table */
struct iblock {
    /* Magic number */
    uint32_t ib_magic;

    /* CRC of the block */
    uint32_t ib_crc;

    /* Next block */
    uint64_t ib_next;

    /* Inode extents */
    struct iextent ib_blks[LC_IBLOCK_MAX];
};
static_assert(sizeof(struct iblock) == LC_BLOCK_SIZE, "iblock size != LC_BLOCK_SIZE");

/* Emap block entry */
struct emap {
    /* Starting page offset */
    uint64_t e_off;

    /* Starting block number */
    uint64_t e_block;

    /* Count of blocks */
    uint32_t e_count;
} __attribute__((packed));
static_assert(sizeof(struct emap) == 20, "emap size != 20");

/* Number of emap entries in a block */
#define LC_EMAP_BLOCK (LC_BLOCK_SIZE / sizeof(struct emap))

/* Emap block structure */
struct emapBlock {
    /* Magic number */
    uint32_t eb_magic;

    /* Checksum */
    uint32_t eb_crc;

    /* Next block */
    uint64_t eb_next;

    /* Emap entries in a block */
    struct emap eb_emap[LC_EMAP_BLOCK];
};
static_assert(sizeof(struct emapBlock) == LC_BLOCK_SIZE, "emapBlock size != LC_BLOCK_SIZE");

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
