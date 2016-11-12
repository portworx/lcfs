#include "includes.h"

/* Read a file system block */
void *
dfs_readBlock(struct gfs *gfs, struct fs *fs, off_t block) {
    size_t size;
    void *buf;

    //dfs_printf("Reading block %ld\n", block);
    assert((block == DFS_SUPER_BLOCK) || (block < gfs->gfs_super->sb_tblocks));
    posix_memalign(&buf, DFS_BLOCK_SIZE, DFS_BLOCK_SIZE);
    size = pread(gfs->gfs_fd, buf, DFS_BLOCK_SIZE, block * DFS_BLOCK_SIZE);
    assert(size == DFS_BLOCK_SIZE);
    __sync_add_and_fetch(&gfs->gfs_reads, 1);
    __sync_add_and_fetch(&fs->fs_reads, 1);
    return buf;
}

/* Write a file system block */
int
dfs_writeBlock(struct gfs *gfs, struct fs *fs, void *buf, off_t block) {
    size_t count;

    //dfs_printf("dfs_writeBlock: Writing block %ld\n", block);
    assert(block < gfs->gfs_super->sb_tblocks);
    count = pwrite(gfs->gfs_fd, buf, DFS_BLOCK_SIZE, block * DFS_BLOCK_SIZE);
    assert(count == DFS_BLOCK_SIZE);
    __sync_add_and_fetch(&gfs->gfs_writes, 1);
    __sync_add_and_fetch(&fs->fs_writes, 1);
    return 0;
}

/* Write a scatter gather list of buffers */
int
dfs_writeBlocks(struct gfs *gfs, struct fs *fs,
                struct iovec *iov, int iovcnt, off_t block) {
    ssize_t count;

    //dfs_printf("dfs_writeBlocks: Writing %d block %ld\n", iovcnt, block);
    assert(block < gfs->gfs_super->sb_tblocks);
    count = pwritev(gfs->gfs_fd, iov, iovcnt, block * DFS_BLOCK_SIZE);
    assert(count != -1);
    __sync_add_and_fetch(&gfs->gfs_writes, 1);
    __sync_add_and_fetch(&fs->fs_writes, 1);
    return 0;
}
