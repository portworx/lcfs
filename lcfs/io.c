#include "includes.h"

/* Read a file system block */
void
lc_readBlock(struct gfs *gfs, struct fs *fs, off_t block, void *dbuf) {
    size_t size;

    //lc_printf("Reading block %ld\n", block);
    assert((block == LC_SUPER_BLOCK) || (block < gfs->gfs_super->sb_tblocks));
    size = pread(gfs->gfs_fd, dbuf, LC_BLOCK_SIZE, block * LC_BLOCK_SIZE);
    assert(size == LC_BLOCK_SIZE);
    __sync_add_and_fetch(&gfs->gfs_reads, 1);
    __sync_add_and_fetch(&fs->fs_reads, 1);
}

/* Write a file system block */
void
lc_writeBlock(struct gfs *gfs, struct fs *fs, void *buf, off_t block) {
    size_t count;

    //lc_printf("lc_writeBlock: Writing block %ld\n", block);
    assert(block < gfs->gfs_super->sb_tblocks);
    count = pwrite(gfs->gfs_fd, buf, LC_BLOCK_SIZE, block * LC_BLOCK_SIZE);
    assert(count == LC_BLOCK_SIZE);
    __sync_add_and_fetch(&gfs->gfs_writes, 1);
    __sync_add_and_fetch(&fs->fs_writes, 1);
}

/* Write a scatter gather list of buffers */
void
lc_writeBlocks(struct gfs *gfs, struct fs *fs,
               struct iovec *iov, int iovcnt, off_t block) {
    ssize_t count;

    //lc_printf("lc_writeBlocks: Writing %d block %ld\n", iovcnt, block);
    assert(block < gfs->gfs_super->sb_tblocks);
    count = pwritev(gfs->gfs_fd, iov, iovcnt, block * LC_BLOCK_SIZE);
    assert(count == (iovcnt * LC_BLOCK_SIZE));
    __sync_add_and_fetch(&gfs->gfs_writes, 1);
    __sync_add_and_fetch(&fs->fs_writes, 1);
}

/* Validate crc of the block read */
void
lc_verifyBlock(void *buf, uint32_t *crc) {
    uint32_t old = *crc, new;

    *crc = 0;
    new = crc32(0, (Bytef *)buf, LC_BLOCK_SIZE);
    assert(old == new);
    *crc = new;
}

/* Calculate and store new crc for a block */
void
lc_updateCRC(void *buf, uint32_t *crc) {
    *crc = 0;
    *crc = crc32(0, (Bytef *)buf, LC_BLOCK_SIZE);
}
