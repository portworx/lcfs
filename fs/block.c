#include "includes.h"

/* Advance block allocator with number of blocks allocated */
void
dfs_blockAlloc(struct fs *fs, int count) {
    __sync_add_and_fetch(&fs->fs_gfs->gfs_super->sb_nblock, count);
}

/* Free file system blocks */
void
dfs_blockFree(struct gfs *gfs, uint64_t count) {
    __sync_sub_and_fetch(&gfs->gfs_super->sb_nblock, count);
}

