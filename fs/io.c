#include "includes.h"

/* Read a file system block */
void *
dfs_readBlock(int fd, off_t block) {
    size_t size;
    void *buf;

    /* XXX assert block is within file system */
    //dfs_printf("Reading block %ld\n", block);
    posix_memalign(&buf, DFS_BLOCK_SIZE, DFS_BLOCK_SIZE);
    size = pread(fd, buf, DFS_BLOCK_SIZE, block * DFS_BLOCK_SIZE);
    assert(size == DFS_BLOCK_SIZE);
    return buf;
}

/* Write a file system block */
int
dfs_writeBlock(int fd, void *buf, off_t block) {
    size_t count;

    /* XXX Use async I/O */
    /* XXX assert block is within file system */
    //dfs_printf("dfs_writeBlock: Writing block %ld\n", block);
    count = pwrite(fd, buf, DFS_BLOCK_SIZE, block * DFS_BLOCK_SIZE);
    assert(count == DFS_BLOCK_SIZE);
    return 0;
}

/* Write a scatter gather list of buffers */
int
dfs_writeBlocks(int fd, struct iovec *iov, int iovcnt, off_t block) {
    ssize_t count;

    /* XXX assert block is within file system */
    //dfs_printf("dfs_writeBlocks: Writing %d block %ld\n", iovcnt, block);
    count = pwritev(fd, iov, iovcnt, block * DFS_BLOCK_SIZE);
    assert(count != -1);
    return 0;
}
