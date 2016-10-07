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

    count = pwrite(fd, buf, DFS_BLOCK_SIZE, block * DFS_BLOCK_SIZE);
    if (count != DFS_BLOCK_SIZE) {
        perror("pwrite");
        exit(errno);
    }
    return 0;
}
