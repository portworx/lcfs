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

/* File system superblock */
struct super {

    /* A magic number */
    uint32_t sb_magic;

    /* Version of the file system layout */
    uint32_t sb_version;

    /* Number of times file system mounted */
    uint64_t sb_mounts;

    /* Total number of file system blocks */
    uint64_t sb_tblocks;

    /* Next block available for allocation */
    uint64_t sb_nblock;

    /* Next inode available for allocation */
    uint64_t sb_ninode;

    /* Padding for filling up a block */
    uint8_t  sb_pad[DFS_BLOCK_SIZE - 40];
};

#endif
