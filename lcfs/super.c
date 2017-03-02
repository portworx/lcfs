#include "includes.h"

/* Initialize a superblock */
void
lc_superInit(struct super *super, uint64_t root, size_t size,
             uint32_t flags, bool global) {
    memset(super, 0, sizeof(struct super));
    super->sb_magic = LC_SUPER_MAGIC;
    super->sb_version = LC_VERSION;
    super->sb_inodeBlock = LC_INVALID_BLOCK;
    super->sb_extentBlock = LC_INVALID_BLOCK;
    super->sb_ftypes[LC_FTYPE_DIRECTORY] = 1;
    super->sb_root = root;
    super->sb_flags = flags;
    if (global) {

        /* These fields are updated in the global superblock only */
        super->sb_blocks = LC_START_BLOCK;
        super->sb_ninode = LC_START_INODE;
        super->sb_inodes = 1;
        super->sb_tblocks = size / LC_BLOCK_SIZE;
    }
}

/* Check if superblock is valid or not */
bool
lc_superValid(struct super *super) {
    return (super->sb_magic == LC_SUPER_MAGIC) &&
           (super->sb_version == LC_VERSION);
}

/* Read file system super block */
void
lc_superRead(struct gfs *gfs, struct fs *fs, uint64_t block) {
    struct super *super;

    lc_mallocBlockAligned(fs, (void **)&super, LC_MEMTYPE_BLOCK);
    lc_readBlock(gfs, fs, block, super);

    /* Verify checksum if a valid super block is found */
    if (lc_superValid(super)) {
        lc_verifyBlock(super, &super->sb_crc);
    }
    fs->fs_super = super;
}

/* Write out file system superblock */
void
lc_superWrite(struct gfs *gfs, struct fs *fs) {
    struct super *super = fs->fs_super;

    /* Update checksum before writing to disk */
    lc_updateCRC(super, &super->sb_crc);
    lc_writeBlock(gfs, fs, super, fs->fs_sblock);
}

/* Mark super block dirty */
void
lc_markSuperDirty(struct fs *fs, bool write) {
    struct gfs *gfs;

    fs->fs_super->sb_flags |= LC_SUPER_DIRTY;
    if (write && !fs->fs_dirty) {
        gfs = fs->fs_gfs;
        pthread_mutex_lock(&gfs->gfs_flock);
        if (!fs->fs_dirty) {
            lc_superWrite(gfs, fs);
            fs->fs_dirty = true;
        }
        pthread_mutex_unlock(&gfs->gfs_flock);
    } else {
        fs->fs_dirty = true;
    }
}
