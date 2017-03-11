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

    assert(fs->fs_dirty);

    /* Update checksum before writing to disk */
    lc_updateCRC(super, &super->sb_crc);
    lc_writeBlock(gfs, fs, super, fs->fs_sblock);
    fs->fs_dirty = false;
}

/* Allocate superblocks for layers as needed */
void
lc_allocateSuperBlocks(struct gfs *gfs, struct fs *rfs, bool write) {
    uint64_t block;
    struct fs *fs;
    int i, count;

    /* Check if superblock of any layers is dirty */
    for (i = 1; i <= gfs->gfs_scount; i++) {
        fs = gfs->gfs_fs[i];
        if (fs && fs->fs_dirty) {
            break;
        }
    }
    if ((i > gfs->gfs_scount) && !rfs->fs_dirty) {
        return;
    }
    lc_markSuperDirty(rfs);

    /* Allocate new superblocks for all layers */
    count = gfs->gfs_count - 1;
    block = count ?
            lc_blockAllocExact(rfs, count, true, false) : LC_INVALID_BLOCK;
    for (i = 1; i <= gfs->gfs_scount; i++) {
        fs = gfs->gfs_fs[i];
        if (fs) {
            if (fs->fs_sblock != LC_INVALID_BLOCK) {
                lc_addFreedBlocks(rfs, fs->fs_sblock, 1, true);
            }
            fs->fs_sblock = block++;
            lc_markSuperDirty(fs);
            count--;
        }
    }
    assert(count == 0);

    /* Link the newly allocated super blocks */
    for (i = 0; i <= gfs->gfs_scount; i++) {
        fs = gfs->gfs_fs[i];
        if (fs) {
            fs->fs_super->sb_nextLayer = fs->fs_next ?
                                            fs->fs_next->fs_sblock : 0;
            fs->fs_super->sb_childLayer = fs->fs_child ?
                                            fs->fs_child->fs_sblock : 0;

            /* Write the superblock if it is pending write */
            if (i && write) {
                lc_lock(fs, true);

                /* XXX Avoid synchronous writes */
                lc_superWrite(gfs, fs);
                lc_unlock(fs);
            } else {
                assert(fs->fs_dirty);
            }
        }
    }
}
