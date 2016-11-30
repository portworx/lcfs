#include "includes.h"

/* Read a file system block */
void *
lc_readBlock(struct gfs *gfs, struct fs *fs, off_t block, void *dbuf) {
    size_t size;
    void *buf;

    //lc_printf("Reading block %ld\n", block);
    assert((block == LC_SUPER_BLOCK) || (block < gfs->gfs_super->sb_tblocks));
    if (dbuf == NULL) {
        malloc_aligned((void **)&buf);
    } else {
        buf = dbuf;
    }
    size = pread(gfs->gfs_fd, buf, LC_BLOCK_SIZE, block * LC_BLOCK_SIZE);
    assert(size == LC_BLOCK_SIZE);
    __sync_add_and_fetch(&gfs->gfs_reads, 1);
    __sync_add_and_fetch(&fs->fs_reads, 1);
    return buf;

}

/* Write a file system block */
int
lc_writeBlock(struct gfs *gfs, struct fs *fs, void *buf, off_t block) {
    size_t count;

    //lc_printf("lc_writeBlock: Writing block %ld\n", block);
    assert(block < gfs->gfs_super->sb_tblocks);
    count = pwrite(gfs->gfs_fd, buf, LC_BLOCK_SIZE, block * LC_BLOCK_SIZE);
    assert(count == LC_BLOCK_SIZE);
    __sync_add_and_fetch(&gfs->gfs_writes, 1);
    __sync_add_and_fetch(&fs->fs_writes, 1);
    return 0;
}

/* Write a scatter gather list of buffers */
int
lc_writeBlocks(struct gfs *gfs, struct fs *fs,
                struct iovec *iov, int iovcnt, off_t block) {
    ssize_t count;

    //lc_printf("lc_writeBlocks: Writing %d block %ld\n", iovcnt, block);
    assert(block < gfs->gfs_super->sb_tblocks);
    count = pwritev(gfs->gfs_fd, iov, iovcnt, block * LC_BLOCK_SIZE);
    assert(count != -1);
    __sync_add_and_fetch(&gfs->gfs_writes, 1);
    __sync_add_and_fetch(&fs->fs_writes, 1);
    return 0;
}
