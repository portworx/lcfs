#include "includes.h"

static bool stats_enabled = true;

/* Type of requests tracked in stats */
static const char *requests[] = {
    "LOOKUP",
    "GETATTR",
    "SETATTR",
    "READLINK",
    "MKNOD",
    "MKDIR",
    "UNLINK",
    "RMDIR",
    "SYMLINK",
    "RENAME",
    "LINK",
    "OPEN",
    "READ",
    "FLUSH",
    "RELEASE",
    "FSYNC",
    "OPENDIR",
    "READDIR",
    "RELEASEDIR",
    "FSYNCDIR",
    "STATFS",
    "SETXATTR",
    "GETXATTR",
    "LISTXATTR",
    "REMOVEXATTR",
    "CREATE",
    "WRITE_BUF",
    "CLONE_CREATE",
    "CLONE_REMOVE",
};

/* Allocate a new stats structure */
struct stats *
dfs_statsNew() {
    struct stats *stats = malloc(sizeof(struct stats));
    struct timeval min;
    enum dfs_stats i;

    if (!stats_enabled) {
        return NULL;
    }
    gettimeofday(&min, NULL);
    memset(&stats->s_count, 0, sizeof(uint64_t) * DFS_REQUEST_MAX);
    memset(&stats->s_err, 0, sizeof(uint64_t) * DFS_REQUEST_MAX);
    memset(&stats->s_max, 0, sizeof(struct timeval) * DFS_REQUEST_MAX);
    memset(&stats->s_total, 0, sizeof(struct timeval) * DFS_REQUEST_MAX);
    for (i = 0; i < DFS_REQUEST_MAX; i++) {
        stats->s_min[i] = min;
    }
    pthread_mutex_init(&stats->s_lock, NULL);
    return stats;
}

/* Begin stats tracking for a new request starting */
void
dfs_statsBegin(struct timeval *start) {
    if (stats_enabled) {
        gettimeofday(start, NULL);
    }
}

/* Update stats for the specified request type */
void
dfs_statsAdd(struct fs *fs, enum dfs_stats type, bool err, struct timeval *start) {
    struct stats *stats = fs->fs_stats;
    struct timeval stop, total;

    if (!stats_enabled) {
        return;
    }
    gettimeofday(&stop, NULL);
    timersub(&stop, start, &total);
    pthread_mutex_lock(&stats->s_lock);
    stats->s_count[type]++;
    if (err) {
        stats->s_err[type]++;
    }
    timeradd(&stats->s_total[type], &total, &stats->s_total[type]);
    if (timercmp(&stats->s_max[type], &total, <)) {
        stats->s_max[type] = total;
    }
    if (timercmp(&stats->s_min[type], &total, >)) {
        stats->s_min[type] = total;
    }
    fs->fs_atime = stop.tv_sec;
    pthread_mutex_unlock(&stats->s_lock);
}

/* Display stats of a file system */
void
dfs_displayStats(struct fs *fs) {
    struct stats *stats = fs->fs_stats;
    enum dfs_stats i;

    if (!stats_enabled) {
        return;
    }
    printf("\n\nStats for file system %p with root %ld index %d\n",
           fs, fs->fs_root, fs->fs_gindex);
    printf("Creation time %s", ctime(&fs->fs_ctime));
    printf("Access   time %s\n", ctime(&fs->fs_atime));
    printf("\tRequest:\tTotal\t\tFailed\tAverage\t\tMax\t\tMin\n\n");
    for (i = 0; i < DFS_REQUEST_MAX; i++) {
        if (stats->s_count[i]) {
            printf("%15s: %10ld\t%10ld\t%2lds %6ldu\t%2lds %6ldu\t%2lds %6ldu\n",
                   requests[i], stats->s_count[i], stats->s_err[i],
                   stats->s_total[i].tv_sec / stats->s_count[i],
                   stats->s_total[i].tv_usec / stats->s_count[i],
                   stats->s_max[i].tv_sec, stats->s_max[i].tv_usec,
                   stats->s_min[i].tv_sec, stats->s_min[i].tv_usec);
        }
    }
    printf("\n\n");
}

/* Display stats of all file systems */
void
dfs_displayStatsAll(struct gfs *gfs) {
    int i;

    if (!stats_enabled) {
        return;
    }
    for (i = 0; i <= gfs->gfs_scount; i++) {
        if (gfs->gfs_fs[i]) {
            dfs_displayStats(gfs->gfs_fs[i]);
        }
    }
}

/* Free resources associated with the stats of a file system */
void
dfs_statsDeinit(struct fs *fs) {
    if (stats_enabled) {
        pthread_mutex_destroy(&fs->fs_stats->s_lock);
        free(fs->fs_stats);
    }
}
