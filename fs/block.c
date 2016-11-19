#include "includes.h"

#define LC_META_RESERVE 1024

/* Advance block allocator with number of blocks allocated */
uint64_t
lc_blockAlloc(struct fs *fs, int count, bool meta) {
    __sync_add_and_fetch(&fs->fs_gfs->gfs_super->sb_blocks, count);
    if (meta) {
        assert(count == 1);
        if (fs->fs_meta_count < count) {
            fs->fs_meta_next = __sync_fetch_and_add(
                                    &fs->fs_gfs->gfs_super->sb_nblock,
                                    LC_META_RESERVE);
            fs->fs_meta_count = LC_META_RESERVE;
        }
        fs->fs_meta_count -= count;
        return fs->fs_meta_next++;
    }
    return __sync_fetch_and_add(&fs->fs_gfs->gfs_super->sb_nblock, count);
}

/* Free file system blocks */
void
lc_blockFree(struct gfs *gfs, uint64_t count) {
    __sync_sub_and_fetch(&gfs->gfs_super->sb_blocks, count);
}

