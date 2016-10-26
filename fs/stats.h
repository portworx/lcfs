#ifndef _STATS_H
#define _STATS_H

/* Type of requests */
enum dfs_stats {
    DFS_LOOKUP = 0,
    DFS_GETATTR = 1,
    DFS_SETATTR = 2,
    DFS_READLINK = 3,
    DFS_MKNOD = 4,
    DFS_MKDIR = 5,
    DFS_UNLINK = 6,
    DFS_RMDIR = 7,
    DFS_SYMLINK = 8,
    DFS_RENAME = 9,
    DFS_LINK = 10,
    DFS_OPEN = 11,
    DFS_READ = 12,
    DFS_FLUSH = 13,
    DFS_RELEASE = 14,
    DFS_FSYNC = 15,
    DFS_OPENDIR = 16,
    DFS_READDIR = 17,
    DFS_RELEASEDIR = 18,
    DFS_FSYNCDIR = 19,
    DFS_STATFS = 20,
    DFS_SETXATTR = 21,
    DFS_GETXATTR = 22,
    DFS_LISTXATTR = 23,
    DFS_REMOVEXATTR = 24,
    DFS_CREATE = 25,
    DFS_WRITE_BUF = 26,
    DFS_CLONE_CREATE = 27,
    DFS_CLONE_REMOVE = 28,
    DFS_REQUEST_MAX = 29,
};

/* Structure tracking stats */
struct stats {

    /* Lock protecting stats */
    pthread_mutex_t s_lock;

    /* Count of each requests processed */
    uint64_t s_count[DFS_REQUEST_MAX];

    /* Count of requests failed */
    uint64_t s_err[DFS_REQUEST_MAX];

    /* Maximum time taken by each request */
    struct timeval s_max[DFS_REQUEST_MAX];

    /* Minimum time taken by each request */
    struct timeval s_min[DFS_REQUEST_MAX];

    /* Total time taken by each request */
    struct timeval s_total[DFS_REQUEST_MAX];
};

#endif
