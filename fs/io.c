#include "includes.h"

/* Read a file system block */
void *
dfs_readBlock(int fd, off_t block) {
    size_t size;
    void *buf;

    posix_memalign(&buf, DFS_BLOCK_SIZE, DFS_BLOCK_SIZE);
    size = pread(fd, buf, DFS_BLOCK_SIZE, block * DFS_BLOCK_SIZE);
    if (size != DFS_BLOCK_SIZE) {
        perror("pread");
        exit(errno);
    }
    return buf;
}

/* Write a file system block */
int
dfs_writeBlock(int fd, void *buf, off_t block) {
    size_t count;

    /* XXX Use async I/O */
    count = pwrite(fd, buf, DFS_BLOCK_SIZE, block * DFS_BLOCK_SIZE);
    if (count != DFS_BLOCK_SIZE) {
        perror("pwrite");
        exit(errno);
    }
    return 0;
}

/* Write a scatter gather list of buffers */
int
dfs_writeBlocks(int fd, struct iovec *iov, int iovcnt, off_t block) {
    ssize_t count;

    count = pwritev(fd, iov, iovcnt, block * DFS_BLOCK_SIZE);
    if (count == -1) {
        perror("pwritev");
        exit(errno);
    }
    return 0;
}
