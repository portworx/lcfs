#ifndef _STATS_H
#define _STATS_H

/* Type of requests */
enum lc_stats {
    LC_LOOKUP = 0,
    LC_GETATTR = 1,
    LC_SETATTR = 2,
    LC_READLINK = 3,
    LC_MKNOD = 4,
    LC_MKDIR = 5,
    LC_UNLINK = 6,
    LC_RMDIR = 7,
    LC_SYMLINK = 8,
    LC_RENAME = 9,
    LC_LINK = 10,
    LC_OPEN = 11,
    LC_READ = 12,
    LC_FLUSH = 13,
    LC_RELEASE = 14,
    LC_LCYNC = 15,
    LC_OPENDIR = 16,
    LC_READDIR = 17,
    LC_RELEASEDIR = 18,
    LC_LCYNCDIR = 19,
    LC_STATLC = 20,
    LC_SETXATTR = 21,
    LC_GETXATTR = 22,
    LC_LISTXATTR = 23,
    LC_REMOVEXATTR = 24,
    LC_CREATE = 25,
    LC_WRITE_BUF = 26,
    LC_CLONE_CREATE = 27,
    LC_CLONE_REMOVE = 28,
    LC_MOUNT = 29,
    LC_STAT = 30,
    LC_UMOUNT = 31,
    LC_CLEANUP = 32,
    LC_REQUEST_MAX = 33,
};

/* Structure tracking stats */
struct stats {

    /* Lock protecting stats */
    pthread_mutex_t s_lock;

    /* Count of each requests processed */
    uint64_t s_count[LC_REQUEST_MAX];

    /* Count of requests failed */
    uint64_t s_err[LC_REQUEST_MAX];

    /* Maximum time taken by each request */
    struct timeval s_max[LC_REQUEST_MAX];

    /* Minimum time taken by each request */
    struct timeval s_min[LC_REQUEST_MAX];

    /* Total time taken by each request */
    struct timeval s_total[LC_REQUEST_MAX];
};

#endif
