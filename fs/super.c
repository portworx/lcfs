#include "includes.h"

/* Initialize a superblock */
void
lc_superInit(struct super *super, size_t size, bool global) {
    memset(super, 0, sizeof(struct super));
    super->sb_magic = LC_SUPER_MAGIC;
    super->sb_inodeBlock = LC_INVALID_BLOCK;
    if (global) {
        super->sb_root = LC_ROOT_INODE;
        super->sb_version = LC_VERSION;
        super->sb_nblock = LC_START_BLOCK;
        super->sb_blocks = super->sb_nblock;
        super->sb_ninode = LC_START_INODE;
        super->sb_tblocks = size / LC_BLOCK_SIZE;
    }
}

/* Read file system super block */
struct super *
lc_superRead(struct gfs *gfs, uint64_t block) {
    return (struct super *)lc_readBlock(gfs, lc_getGlobalFs(gfs), block,
                                         NULL);
}

/* Write out file system superblock */
int
lc_superWrite(struct gfs *gfs, struct fs *fs) {
    return lc_writeBlock(gfs, fs, fs->fs_super, fs->fs_sblock);
}
