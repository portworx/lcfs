#include "includes.h"

/* Advance block allocator with number of blocks allocated */
uint64_t
dfs_blockAlloc(struct fs *fs, int count) {
    __sync_add_and_fetch(&fs->fs_gfs->gfs_super->sb_blocks, count);
    return __sync_fetch_and_add(&fs->fs_gfs->gfs_super->sb_nblock, count);
}

/* Free file system blocks */
void
dfs_blockFree(struct gfs *gfs, uint64_t count) {
    __sync_sub_and_fetch(&gfs->gfs_super->sb_blocks, count);
}

