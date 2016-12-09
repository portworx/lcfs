#include "includes.h"

/* Initialize a superblock */
void
lc_superInit(struct super *super, size_t size, bool global) {
    memset(super, 0, sizeof(struct super));
    super->sb_magic = LC_SUPER_MAGIC;
    super->sb_inodeBlock = LC_INVALID_BLOCK;
    super->sb_extentBlock = LC_INVALID_BLOCK;
    if (global) {
        super->sb_root = LC_ROOT_INODE;
        super->sb_version = LC_VERSION;
        super->sb_blocks = LC_START_BLOCK;
        super->sb_ninode = LC_START_INODE;
        super->sb_tblocks = size / LC_BLOCK_SIZE;
    }
}

/* Read file system super block */
void
lc_superRead(struct gfs *gfs, struct fs *fs, uint64_t block) {
    lc_mallocBlockAligned(fs, (void **)&fs->fs_super, LC_MEMTYPE_BLOCK);
    lc_readBlock(gfs, fs, block, fs->fs_super);
}

/* Write out file system superblock */
int
lc_superWrite(struct gfs *gfs, struct fs *fs) {
    return lc_writeBlock(gfs, fs, fs->fs_super, fs->fs_sblock);
}
