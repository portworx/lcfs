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

    /* Version of the file system layout */
    uint32_t sb_version;

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

    /* Padding for filling up a block */
    uint8_t  sb_pad[DFS_BLOCK_SIZE - 104];
};

#endif
