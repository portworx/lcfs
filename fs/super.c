#include "includes.h"

/* Read file system super block */
int
dfs_superRead(struct gfs *gfs) {
    struct super *super;

    super = (struct super *)dfs_readBlock(gfs->gfs_fd, DFS_SUPER_BLOCK);
    printf("version %d magic %d mounts %ld\n", super->sb_version, super->sb_magic, super->sb_mounts);
    gfs->gfs_super = super;
    return 0;
}

/* Write out file system superblock */
int
dfs_superWrite(struct gfs *gfs) {
    return dfs_writeBlock(gfs->gfs_fd, gfs->gfs_super, DFS_SUPER_BLOCK);
}
