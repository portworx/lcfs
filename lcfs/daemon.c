#include "includes.h"
#include "version/version.h"

static struct gfs *gfs;
bool lc_verbose = false;
extern struct fuse_lowlevel_ops lc_ll_oper;

#define LC_SIZEOF_MOUNTARGS 1024

/* Return global file system */
struct gfs *
getfs() {
    return gfs;
}

/* Display usage */
static void
usage(char *prog) {
    lc_syslog(LOG_ERR, "usage: %s daemon <device> <host-mnt> <plugin-mnt>"
                       " [-f] [-c] [-d] [-m] [-r] [-t] [-p] [-s] [-v]\n", prog);
    lc_syslog(LOG_ERR, "\tdevice        - device or file - image layers"
                       " will be saved here\n"
                    "\thost-mnt      - mount point on host\n"
                    "\tplugin-mnt    - mount point propagated to plugin\n"
                    "\t-f            - run foreground (optional)\n"
                    "\t-c            - format file system (optional)\n"
                    "\t-d            - display debugging info (optional)\n"
                    "\t-m            - enable memory stats (optional)\n"
                    "\t-r            - enable request stats (optional)\n"
                    "\t-t            - enable tracking count of file types"
                                       " (optional)\n"
                    "\t-p            - enable profiling (optional)\n"
                    "\t-s            - swap layers when committed\n"
                    "\t-v            - enable verbose mode (optional)\n");
}

/* Notify parent process completion */
static void
lc_notifyParent(int *waiter) {
    char completed = 1;
    int err;

    err = write(waiter[1], &completed, sizeof(completed));
    assert(err == sizeof(completed));
    close(waiter[0]);
    close(waiter[1]);
}

/* Daemonize if run in background mode */
static int
lc_daemonize(int *waiter) {
    int err, nullfd;

    err = setsid();
    if (err == -1) {
        perror("setsid");
        goto out;
    }

    err = chdir("/");
    assert(err == 0);
    nullfd = open("/dev/null", O_RDWR, 0);
    if (nullfd == -1) {
        perror("open");
        err = -1;
        goto out;
    }
    (void) dup2(nullfd, 0);
    (void) dup2(nullfd, 1);
    (void) dup2(nullfd, 2);
    if (nullfd > 2) {
        close(nullfd);
    }
    lc_notifyParent(waiter);

out:
    return err;
}

/* Destroy a fuse session */
static void
lc_stopSession(struct gfs *gfs, struct fuse_session *se, enum lc_mountId id) {
#ifdef FUSE3
    fuse_session_unmount(se);
#else
    fuse_session_remove_chan(gfs->gfs_ch[id]);
#endif
    fuse_session_destroy(se);
    if (id == LC_LAYER_MOUNT) {
        fuse_remove_signal_handlers(se);
    }
#ifndef FUSE3
    fuse_unmount(gfs->gfs_mountpoint[id], gfs->gfs_ch[id]);
#endif
}

/* Start background threads */
static void *
lc_startThreads(void *data) {
    struct gfs *gfs = (struct gfs *)data;
    pthread_t flusher, syncer;
    int err;

    /* Start a thread to flush dirty pages */
    err = pthread_create(&flusher, NULL, lc_flusher, gfs);
    assert(err == 0);

    /* Start a thread to checkpoint file system periodically */
    err = pthread_create(&syncer, NULL, lc_syncer, gfs);
    assert(err == 0);

    /* Flush and purge pages in the background */
    lc_cleaner();

    /* Wait for flusher and syncer to exit */
    pthread_cond_signal(&gfs->gfs_flusherCond);
    pthread_cond_signal(&gfs->gfs_syncerCond);
    pthread_join(syncer, NULL);
    pthread_join(flusher, NULL);
    return NULL;
}

/* Serve file system requests */
static void *
lc_serve(void *data) {
    enum lc_mountId id = (enum lc_mountId)data, other;
    struct gfs *gfs = getfs();
    struct fuse_session *se;
    bool fcancel = false;
    pthread_t cleaner;
    int err = 0;

    if (id == LC_LAYER_MOUNT) {

        err = pthread_create(&cleaner, NULL, lc_startThreads, gfs);
        if (err) {
            lc_syslog(LOG_ERR,
                      "Cleaner thread could not be created, err %d\n", err);
            goto out;
        }
        fcancel = true;
    }

#ifdef FUSE3
    fuse_session_mount(gfs->gfs_se[id], gfs->gfs_mountpoint[id]);
#else
    fuse_session_add_chan(gfs->gfs_se[id], gfs->gfs_ch[id]);
#endif

    /* Daemonize if running in background */
    if ((id == LC_LAYER_MOUNT) && gfs->gfs_waiter) {
        err = lc_daemonize(gfs->gfs_waiter);
        if (err) {
            lc_syslog(LOG_ERR, "Failed to daemonize\n");
        }
    }
    if (!err) {
        err = fuse_session_loop_mt(gfs->gfs_se[id]
#ifdef FUSE3
                                   , 0);
    }
#else
                                   );
    }
#endif

out:
    gfs->gfs_unmounting = true;

    /* Other mount need to exit as well */
    other = (id == LC_BASE_MOUNT) ? LC_LAYER_MOUNT : LC_BASE_MOUNT;
    if (gfs->gfs_se[other]) {
        lc_syslog(LOG_INFO, "Waiting for %s to be unmounted\n",
                  gfs->gfs_mountpoint[other]);
    }
    pthread_mutex_lock(&gfs->gfs_lock);
    se = gfs->gfs_se[other];
    if (se) {
        if ((id == LC_BASE_MOUNT) && (gfs->gfs_mcount == 0)) {
            pthread_cond_signal(&gfs->gfs_mountCond);
        }
        fuse_session_exit(se);
    }
    se = gfs->gfs_se[id];
    gfs->gfs_se[id] = NULL;
    pthread_mutex_unlock(&gfs->gfs_lock);
    if (id == LC_LAYER_MOUNT) {

        /* Wait for base mount to unmount */
        /* XXX Figure out how to make that to exit */
        pthread_join(gfs->gfs_mountThread, NULL);

        /* Wait for cleaner thread to exit */
        if (fcancel) {
            pthread_mutex_lock(&gfs->gfs_lock);
            pthread_cond_signal(&gfs->gfs_cleanerCond);
            pthread_mutex_unlock(&gfs->gfs_lock);
            pthread_join(cleaner, NULL);
        }
    }
    lc_stopSession(gfs, se, id);
    return NULL;
}
/* Start a fuse session after processing the arguments */
static int
lc_fuseSession(struct gfs *gfs, char **arg, int argc, enum lc_mountId id) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, arg);
    struct fuse_session *se;
    char *mountpoint = NULL;
    int err;
#ifdef FUSE3
    struct fuse_cmdline_opts opts;

    err = fuse_parse_cmdline(&args, &opts);
    if (err == -1) {
        fuse_cmdline_help();
        //fuse_lowlevel_help();
        err = EINVAL;
        goto out;
    }
    mountpoint = opts.mountpoint;
    if (opts.show_help) {
        fuse_cmdline_help();
        //fuse_lowlevel_help();
        err = EINVAL;
        goto out;
    }
    if (opts.show_version) {
        syslog(LOG_INFO, "FUSE library version %s\n", fuse_pkgversion());
        fuse_lowlevel_version();
        err = EINVAL;
        goto out;
    }
    se = fuse_session_new
#else
    struct fuse_chan *ch;

    err = fuse_parse_cmdline(&args, &mountpoint, NULL, NULL);
    if (err == -1) {
        err = EINVAL;
        goto out;
    }
    ch = fuse_mount(mountpoint, &args);
    if (!ch) {
        err = EINVAL;
        goto out;
    }
    gfs->gfs_ch[id] = ch;
    se = fuse_lowlevel_new
#endif
                          (&args, &lc_ll_oper, sizeof(lc_ll_oper), gfs);
    if (!se) {
        err = EINVAL;
        goto out;
    }
    gfs->gfs_se[id] = se;
    gfs->gfs_mountpoint[id] = mountpoint;
    mountpoint = NULL;

out:
    if (mountpoint) {
        lc_free(NULL, mountpoint, 0, LC_MEMTYPE_GFS);
    }
    fuse_opt_free_args(&args);
    return err;
}

/* Start file system services on mount points */
static int
lc_start(struct gfs *gfs, char *device, enum lc_mountId id) {
    int err;

    if (id == LC_BASE_MOUNT) {
        err = pthread_create(&gfs->gfs_mountThread, NULL,
                             lc_serve, (void *)id);
        if (err) {
            perror("pthread_create");
        } else {
            lc_syslog(LOG_INFO, "%s mounted at %s\n",
                      device, gfs->gfs_mountpoint[id]);
        }
    } else {

        /* Wait for first thread to complete */
        err = EIO;
        if (!gfs->gfs_unmounting) {
            pthread_mutex_lock(&gfs->gfs_lock);
            while ((gfs->gfs_mcount == 0) && !gfs->gfs_unmounting) {
                pthread_cond_wait(&gfs->gfs_mountCond, &gfs->gfs_lock);
            }
            pthread_mutex_unlock(&gfs->gfs_lock);
            if (!gfs->gfs_unmounting) {
                lc_syslog(LOG_INFO, "%s mounted at %s\n",
                          device, gfs->gfs_mountpoint[id]);
                lc_serve((void *)id);
                err = 0;
            }
        }
        if (err) {
            lc_syslog(LOG_ERR, "Aborting mount, base layer unmounted\n");
            err = EIO;
        }
    }
    return err;
}

/* Mount the specified device and start serving requests */
int
lcfs_main(char *pgm, int argc, char *argv[]) {
    bool daemon = true, profiling = false, ftypes = false, swap = false;
    int i, err = -1, waiter[2], fd, count;
    char *arg[argc + 1], completed;
    struct fuse_session *se;
    bool format = false;
    struct stat st;
    size_t size;

    openlog("lcfs", LOG_PID|LOG_CONS|LOG_PERROR, LOG_USER);

    /* Validate arguments */
#ifdef FUSE3
    if (argc < 4) {
#else
    if ((argc < 4) || (argc > 6)) {
#endif
        usage(pgm);
        closelog();
        exit(EINVAL);
    }

    if (!strcmp(argv[2], argv[3])) {
        lc_syslog(LOG_ERR, "Specify different mount points\n");
        usage(pgm);
        closelog();
        exit(EINVAL);
    }

    /* Make sure mount points exist */
    if (stat(argv[2], &st) || stat(argv[3], &st)) {
        perror("stat");
        lc_syslog(LOG_ERR,
                "Make sure directories %s and %s exist\n", argv[2], argv[3]);
        usage(pgm);
        closelog();
        exit(errno);
    }

    /* Open the device for mounting */
    fd = lc_deviceOpen(argv[1]);
    if (fd == -1) {
        perror("open");
        lc_syslog(LOG_ERR, "Failed to open %s\n", argv[1]);
        closelog();
        exit(errno);
    }

    /* Find the size of the device */
    size = lseek(fd, 0, SEEK_END);
    if (size == -1) {
        perror("lseek");
        lc_syslog(LOG_ERR, "lseek failed on %s\n", argv[1]);
        close(fd);
        closelog();
        exit(errno);
    }

    if ((size / LC_BLOCK_SIZE) < LC_MIN_BLOCKS) {
        lc_syslog(LOG_ERR,
                "Device is too small. Minimum size required is %ldMB\n",
                (LC_MIN_BLOCKS * LC_BLOCK_SIZE) / (1024 * 1024) + 1);
        close(fd);
        closelog();
        exit(EINVAL);
    }
    if ((size / LC_BLOCK_SIZE) >= LC_MAX_BLOCKS) {
        lc_syslog(LOG_ERR,
                "Device is too big. Maximum size supported is %ldMB\n",
                (LC_MAX_BLOCKS * LC_BLOCK_SIZE) / (1024 * 1024));
        close(fd);
        closelog();
        exit(EINVAL);
    }

    count = 4;
    for (i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "-m")) {
            lc_memStatsEnable();
        } else if (!strcmp(argv[i], "-c")) {
            format = true;
        } else if (!strcmp(argv[i], "-r")) {
            lc_statsEnable();
        } else if (!strcmp(argv[i], "-t")) {
            ftypes = true;
        } else if (!strcmp(argv[i], "-p")) {
            profiling = true;
        } else if (!strcmp(argv[i], "-s")) {
            lc_syslog("WARNING: Enabling layer swapping will break "
                      "docker save/push operations");
            swap = true;
        } else if (!strcmp(argv[i], "-v")) {
            lc_verbose = true;
        } else {
            if (!strcmp(argv[i], "-f") ||
                !strcmp(argv[i], "-d")) {
                daemon = false;
            }
            arg[count++] = argv[i];
        }
    }
    for (i = count; i < argc; i++) {
        arg[i] = NULL;
    }

    /* Fork a new process if run in background mode */
    if (daemon) {
        err = pipe(waiter);
        if (err) {
            perror("pipe");
            close(fd);
            closelog();
            exit(errno);
        }
        switch (fork()) {
        case -1:
            perror("fork");
            close(fd);
            closelog();
            exit(errno);

        case 0:
            break;

        default:

            /* Wait for the mount to complete */
            err = read(waiter[0], &completed, sizeof(completed));
            closelog();
            exit(0);
        }
    }
    lc_syslog(LOG_INFO, "%s %s\n", Build, Release);

    /* Initialize memory allocator */
    lc_memoryInit(0);

    /* Allocate gfs structure */
    gfs = lc_malloc(NULL, sizeof(struct gfs), LC_MEMTYPE_GFS);
    memset(gfs, 0, sizeof(struct gfs));
    if (daemon) {
        gfs->gfs_waiter = waiter;
    }
    gfs->gfs_fd = fd;
    gfs->gfs_profiling = profiling;
    gfs->gfs_swapLayersForCommit = swap;

    /* Setup arguments for fuse mount */
    arg[0] = pgm;
    arg[1] = argv[2];
    arg[2] = "-o";
    arg[3] = lc_malloc(NULL, LC_SIZEOF_MOUNTARGS, LC_MEMTYPE_GFS);
    sprintf(arg[3], "allow_other,noatime,default_permissions,"
#ifndef __APPLE__
                    "auto_unmount,"
#endif
#ifndef FUSE3
#ifndef __APPLE__
                    "nonempty,"
#endif
                    "atomic_o_trunc,big_writes,"
                    "splice_move,splice_read,splice_write,"
#endif
                    "suid,dev,subtype=lcfs,fsname=%s", argv[1]);

    /* Start fuse sessions for the given mount points */
    err = lc_fuseSession(gfs, arg, count, LC_BASE_MOUNT);
    if (err) {
        goto out;
    }
    arg[1] = argv[3];
    err = lc_fuseSession(gfs, arg, count, LC_LAYER_MOUNT);
    if (err) {
        goto out;
    }

    /* Mask signals before mounting the file system */
    se = gfs->gfs_se[LC_LAYER_MOUNT];
    if (fuse_set_signal_handlers(se) == -1) {
        lc_syslog(LOG_ERR, "Error setting signal handlers\n");
        err = EPERM;
        goto out;
    }

    /* Set up the file system before starting services */
    lc_mount(gfs, argv[1], ftypes, size, format);

    /* Start file system services on the mount points */
    for (i = 0; i < LC_MAX_MOUNTS; i++) {
        err = lc_start(gfs, argv[1], i);
        if (err) {
            break;
        }
    }
    if (err && !gfs->gfs_unmounting) {
        gfs->gfs_unmounting = true;
        lc_unmount(gfs);
    }
    assert(gfs->gfs_unmounting);

out:
    if (err) {
        if (daemon) {
            lc_notifyParent(waiter);
        }
    } else {
        lc_syslog(LOG_INFO, "%s unmounted\n", argv[1]);
    }
    for (i = 0; i < LC_MAX_MOUNTS; i++) {
        if (gfs->gfs_se[i]) {
            assert(err);
            lc_stopSession(gfs, gfs->gfs_se[i], i);
        }
        if (gfs->gfs_mountpoint[i]) {
            lc_free(NULL, gfs->gfs_mountpoint[i], 0, LC_MEMTYPE_GFS);
        }
    }
    lc_free(NULL, arg[3], LC_SIZEOF_MOUNTARGS, LC_MEMTYPE_GFS);
    close(fd);
    lc_free(NULL, gfs, sizeof(struct gfs), LC_MEMTYPE_GFS);
    lc_displayGlobalMemStats();
    closelog();
    return err ? 1 : 0;
}
