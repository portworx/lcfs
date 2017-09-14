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

/* Read into a scatter gather list of buffers */
void
lc_readBlocks(struct gfs *gfs, struct fs *fs,
              struct iovec *iov, int iovcnt, off_t block) {
    size_t size;

    //lc_printf("lc_readBlocks: Reading %d blocks %ld\n", iovcnt, block);
    assert((block + iovcnt) < gfs->gfs_super->sb_tblocks);
    size = lc_preadv(gfs->gfs_fd, iov, iovcnt, block * LC_BLOCK_SIZE);
    assert(size == (iovcnt * LC_BLOCK_SIZE));
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

    //lc_printf("lc_writeBlocks: Writing %d blocks %ld\n", iovcnt, block);
    assert((block + iovcnt) < gfs->gfs_super->sb_tblocks);
    if (fs->fs_removed) {
        return;
    }
    count = lc_pwritev(gfs->gfs_fd, iov, iovcnt, block * LC_BLOCK_SIZE);
    assert(count == (iovcnt * LC_BLOCK_SIZE));
    __sync_add_and_fetch(&gfs->gfs_writes, 1);
    __sync_add_and_fetch(&fs->fs_writes, 1);
}

/* Calculate checksum of a block of data */
uint32_t
lc_checksum_sw(char *buf) {
    return crc32(0, (Bytef *)buf, LC_BLOCK_SIZE);
}

/* Calculate checksum of a block of data */
uint32_t
lc_checksum_hw(char *buf) {
    uint32_t hash = 0, i, *data = (uint32_t *)buf;

    for (i = 0; i < (LC_BLOCK_SIZE / sizeof(uint32_t)); i++) {
        hash = _mm_crc32_u32(hash, data[i]);
    }
    return hash;
}

/* Check if cpu supports checksum functionality */
static void *
lc_resolve_checksum(void) {
    __builtin_cpu_init();
    return __builtin_cpu_supports("sse4.2") ? lc_checksum_hw : lc_checksum_sw;
}

/* Calculate checksum of a block of data */
uint32_t
lc_checksum(char *buf) __attribute__ ((ifunc("lc_resolve_checksum")));

/* Validate crc of the block read */
void
lc_verifyBlock(void *buf, uint32_t *crc) {
    uint32_t old = *crc, new;

    *crc = 0;
    new = lc_checksum(buf);
    assert(old == new);
    *crc = new;
}

/* Calculate and store new crc for a block */
void
lc_updateCRC(void *buf, uint32_t *crc) {
    *crc = 0;
    *crc = lc_checksum(buf);
}
