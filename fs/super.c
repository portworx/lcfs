#include "includes.h"

/* Initialize a superblock */
void
dfs_superInit(struct super *super, size_t size, bool global) {
    memset(super, 0, sizeof(struct super));
    super->sb_magic = DFS_SUPER_MAGIC;
    super->sb_inodeBlock = DFS_INVALID_BLOCK;
    if (global) {
        super->sb_root = DFS_ROOT_INODE;
        super->sb_version = DFS_VERSION;
        super->sb_nblock = DFS_START_BLOCK;
        super->sb_blocks = super->sb_nblock;
        super->sb_ninode = DFS_START_INODE;
        super->sb_tblocks = size / DFS_BLOCK_SIZE;
    }
}

/* Read file system super block */
struct super *
dfs_superRead(struct gfs *gfs, uint64_t block) {
    return (struct super *)dfs_readBlock(gfs->gfs_fd, block);
}

/* Write out file system superblock */
int
dfs_superWrite(struct gfs *gfs, struct fs *fs) {
    return dfs_writeBlock(gfs->gfs_fd, fs->fs_super, fs->fs_sblock);
}
