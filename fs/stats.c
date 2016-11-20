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
    "LCYNC",
    "OPENDIR",
    "READDIR",
    "RELEASEDIR",
    "LCYNCDIR",
    "STATLC",
    "SETXATTR",
    "GETXATTR",
    "LISTXATTR",
    "REMOVEXATTR",
    "CREATE",
    "WRITE_BUF",
    "CLONE_CREATE",
    "CLONE_REMOVE",
    "MOUNT",
    "STAT",
    "UMOUNT",
    "CLEANUP",
};

/* Allocate a new stats structure */
struct stats *
lc_statsNew() {
    struct stats *stats;
    struct timeval min;
    enum lc_stats i;

    if (!stats_enabled) {
        return NULL;
    }
    gettimeofday(&min, NULL);
    stats = malloc(sizeof(struct stats));
    memset(&stats->s_count, 0, sizeof(uint64_t) * LC_REQUEST_MAX);
    memset(&stats->s_err, 0, sizeof(uint64_t) * LC_REQUEST_MAX);
    memset(&stats->s_max, 0, sizeof(struct timeval) * LC_REQUEST_MAX);
    memset(&stats->s_total, 0, sizeof(struct timeval) * LC_REQUEST_MAX);
    for (i = 0; i < LC_REQUEST_MAX; i++) {
        stats->s_min[i] = ((i == LC_FLUSH) || (i == LC_LCYNCDIR) ||
                           (i == LC_LCYNC)) ?  (struct timeval){0, 0} : min;
    }
    pthread_mutex_init(&stats->s_lock, NULL);
    return stats;
}

/* Begin stats tracking for a new request starting */
void
lc_statsBegin(struct timeval *start) {
    if (stats_enabled) {
        gettimeofday(start, NULL);
    }
}

/* Update stats for the specified request type */
void
lc_statsAdd(struct fs *fs, enum lc_stats type, bool err,
             struct timeval *start) {
    struct stats *stats = fs->fs_stats;
    struct timeval stop, total;

    if (!stats_enabled) {
        return;
    }

    /* Times are not tracked for certain type of operations */
    if (start == NULL) {
        __sync_add_and_fetch(&stats->s_count[type], 1);
        if (err) {
            __sync_add_and_fetch(&stats->s_err[type], 1);
        }
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
lc_displayStats(struct fs *fs) {
    struct stats *stats = fs->fs_stats;
    struct timeval now;
    enum lc_stats i;

    if (!stats_enabled) {
        return;
    }
    gettimeofday(&now, NULL);
    printf("\n\nStats for file system %p with root %ld index %d at %s\n",
           fs, fs->fs_root, fs->fs_gindex, ctime(&now.tv_sec));
    printf("Layer  created at %s", ctime(&fs->fs_ctime));
    printf("Last acccessed at %s\n", ctime(&fs->fs_atime));
    printf("\tRequest:\tTotal\t\tFailed\tAverage\t\tMax\t\tMin\n\n");
    for (i = 0; i < LC_REQUEST_MAX; i++) {
        if (stats->s_count[i]) {
            printf("%15s: %10ld\t%10ld\t%2lds.%06ldu\t%2lds.%06ldu\t%2lds.%06ldu\n",
                   requests[i], stats->s_count[i], stats->s_err[i],
                   stats->s_total[i].tv_sec / stats->s_count[i],
                   stats->s_total[i].tv_usec / stats->s_count[i],
                   stats->s_max[i].tv_sec, stats->s_max[i].tv_usec,
                   stats->s_min[i].tv_sec, stats->s_min[i].tv_usec);
        }
    }
    printf("\n\n");
    printf("%ld inodes %ld pages\n", fs->fs_icount, fs->fs_pcount);
    printf("%ld reads %ld writes (%ld inodes written)\n",
           fs->fs_reads, fs->fs_writes, fs->fs_iwrite);
    printf("\n\n");
}

/* Display stats of all file systems */
void
lc_displayStatsAll(struct gfs *gfs) {
    int i;

    if (!stats_enabled) {
        return;
    }
    for (i = 0; i <= gfs->gfs_scount; i++) {
        if (gfs->gfs_fs[i]) {
            lc_displayStats(gfs->gfs_fs[i]);
        }
    }
}

/* Display global stats */
void
lc_displayGlobalStats(struct gfs *gfs) {
    printf("Total %ld reads %ld writes\n", gfs->gfs_reads, gfs->gfs_writes);
    printf("%ld inodes cloned\n", gfs->gfs_clones);
    printf("%ld pages hit %ld pages missed "
           "%ld pages recycled %ld pages reused\n", gfs->gfs_phit,
           gfs->gfs_pmissed, gfs->gfs_precycle, gfs->gfs_preused);
}

/* Free resources associated with the stats of a file system */
void
lc_statsDeinit(struct fs *fs) {
    if (stats_enabled) {
        pthread_mutex_destroy(&fs->fs_stats->s_lock);
        free(fs->fs_stats);
    }
}
