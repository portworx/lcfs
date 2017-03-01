#include "includes.h"

/* Set this for tracking total count of various requests and the time taken for
 * processing those requests.
 */
#ifdef LC_STATS_ENABLE
static bool stats_enabled = true;
#else
static bool stats_enabled = false;
#endif

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
    "READDIR_PLUS",
    "LAYER_CREATE",
    "LAYER_REMOVE",
    "MOUNT",
    "STAT",
    "UMOUNT",
    "CLEANUP",
};

/* Allocate a new stats structure */
void
lc_statsNew(struct fs *fs) {
    struct stats *stats;
    struct timeval min;
    enum lc_stats i;

    if (!stats_enabled) {
        return;
    }
    gettimeofday(&min, NULL);
    stats = lc_malloc(fs, sizeof(struct stats), LC_MEMTYPE_STATS);
    memset(&stats->s_count, 0, sizeof(uint64_t) * LC_REQUEST_MAX);
    memset(&stats->s_err, 0, sizeof(uint64_t) * LC_REQUEST_MAX);
    memset(&stats->s_max, 0, sizeof(struct timeval) * LC_REQUEST_MAX);
    memset(&stats->s_total, 0, sizeof(struct timeval) * LC_REQUEST_MAX);
    for (i = 0; i < LC_REQUEST_MAX; i++) {

        /* Time is not tracked for certain requests */
        stats->s_min[i] = ((i == LC_FLUSH) || (i == LC_FSYNCDIR) ||
                           (i == LC_FSYNC)) ?  (struct timeval){0, 0} : min;
    }
    pthread_mutex_init(&stats->s_lock, NULL);
    fs->fs_stats = stats;
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

    /* Calculate time taken to process this request and update stats */
    gettimeofday(&stop, NULL);
    timersub(&stop, start, &total);
    pthread_mutex_lock(&stats->s_lock);

    /* Increment total count */
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

    /* Update layer access time */
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
    printf("\tLayer  created at %s", ctime(&fs->fs_ctime));
    printf("\tLast acccessed at %s\n", ctime(&fs->fs_atime));
    if (stats == NULL) {
        goto out;
    }
    printf("\tRequest:\tTotal\t\tFailed\tAverage\t\tMax\t\tMin\n\n");
    for (i = 0; i < LC_REQUEST_MAX; i++) {
        if (stats->s_count[i]) {
            printf("%15s: %10ld\t%10ld\t%2lds.%06ldu\t%2lds.%06ldu\t"
                   "%2lds.%06ldu\n",
                   requests[i], stats->s_count[i], stats->s_err[i],
                   stats->s_total[i].tv_sec / stats->s_count[i],
                   stats->s_total[i].tv_usec / stats->s_count[i],
                   stats->s_max[i].tv_sec, stats->s_max[i].tv_usec,
                   stats->s_min[i].tv_sec, stats->s_min[i].tv_usec);
        }
    }
    printf("\n\n");

out:
    lc_displayFtypeStats(fs);
    lc_displayAllocStats(fs);
    printf("\t%ld inodes %ld pages\n", fs->fs_icount, fs->fs_pcount);
    printf("\t%ld reads %ld writes (%ld inodes written)\n",
           fs->fs_reads, fs->fs_writes, fs->fs_iwrite);
    printf("\n\n");
}

/* Display stats of a layer */
void
lc_displayLayerStats(struct fs *fs) {
    lc_displayMemStats(fs);
    lc_displayStats(fs);
}

/* Display stats of all file systems */
void
lc_displayStatsAll(struct gfs *gfs) {
    struct fs *fs;
    int i;

    rcu_register_thread();
    rcu_read_lock();
    for (i = 0; i <= gfs->gfs_scount; i++) {
        fs = rcu_dereference(gfs->gfs_fs[i]);
        if (fs) {
            if (i == 0) {
                lc_displayGlobalMemStats();
            }
            lc_displayLayerStats(fs);
        }
    }
    rcu_read_unlock();
    rcu_unregister_thread();
}

/* Display global stats */
void
lc_displayGlobalStats(struct gfs *gfs) {
    uint64_t avail = gfs->gfs_super->sb_tblocks - gfs->gfs_super->sb_blocks;

    printf("Blocks free %ld (%ld%%) used %ld (%ld%%) total %ld\n", avail,
           (avail * 100ul) / gfs->gfs_super->sb_tblocks,
           gfs->gfs_super->sb_blocks,
           (gfs->gfs_super->sb_blocks * 100ul) / gfs->gfs_super->sb_tblocks,
           gfs->gfs_super->sb_tblocks);
    if (gfs->gfs_reads || gfs->gfs_writes) {
        printf("Total %ld reads %ld writes\n",
               gfs->gfs_reads, gfs->gfs_writes);
    }
    if (gfs->gfs_clones) {
        printf("%ld inodes cloned\n", gfs->gfs_clones);
    }
    if (gfs->gfs_phit || gfs->gfs_pmissed || gfs->gfs_precycle ||
        gfs->gfs_preused || gfs->gfs_purged) {
        printf("pages %ld hit %ld missed %ld recycled %ld reused %ld purged\n",
               gfs->gfs_phit, gfs->gfs_pmissed, gfs->gfs_precycle,
               gfs->gfs_preused, gfs->gfs_purged);
    }
}

/* Free resources associated with the stats of a file system */
void
lc_statsDeinit(struct fs *fs) {
    if (stats_enabled) {
        lc_displayStats(fs);
#ifdef LC_MUTEX_DESTROY
        pthread_mutex_destroy(&fs->fs_stats->s_lock);
#endif
        lc_free(fs, fs->fs_stats, sizeof(struct stats), LC_MEMTYPE_STATS);
    } else {
        assert(fs->fs_stats == NULL);
    }
}
