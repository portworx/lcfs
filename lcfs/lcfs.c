#include "includes.h"
#include "version/version.h"

static struct gfs *gfs;
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
    fprintf(stderr, "usage: %s <device> <mnt> <mnt2> [-f] [-d]\n", prog);
    fprintf(stderr, "\tdevice - device/file\n"
                    "\tmnt    - mount point on host\n"
                    "\tmnt2   - mount point propagated to plugin\n"
                    "\t-f     - run foreground (optional)\n"
                    "\t-d     - display debugging info (optional)\n");
}

/* Daemonize if run in background mode */
static int
lc_daemonize(int *waiter) {
    char completed = 1;
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
    err = write(waiter[1], &completed, sizeof(completed));
    close(waiter[0]);
    close(waiter[1]);
    err = 0;

out:
    return err;
}

/* Serve file system requests */
static void *
lc_serve(void *data) {
    enum lc_mountId id = (enum lc_mountId)data;
    struct gfs *gfs = getfs();
    struct fuse_session *se;
    bool fcancel = false;
    pthread_t cleaner;
    int err = 0;

    if (id == LC_LAYER_MOUNT) {
        if (fuse_set_signal_handlers(gfs->gfs_se[id]) == -1) {
            fprintf(stderr, "Error setting signal handlers\n");
            err = EPERM;
            goto out;
        }

        /* Start a background thread to flush and purge pages */
        err = pthread_create(&cleaner, NULL, lc_cleaner, NULL);
        if (err) {
            fprintf(stderr,
                   "Flusher thread could not be created, err %d\n", err);
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
            fprintf(stderr, "Failed to daemonize\n");
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
    fuse_session_remove_chan(gfs->gfs_ch[id]);
#endif

out:
    gfs->gfs_unmounting = true;

    /* Other mount need to exit as well */
    pthread_mutex_lock(&gfs->gfs_lock);
    se = gfs->gfs_se[(id == LC_BASE_MOUNT) ? LC_LAYER_MOUNT : LC_BASE_MOUNT];
    if (se) {
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
        fuse_remove_signal_handlers(se);
    }
#ifdef FUSE3
    fuse_session_unmount(se);
#endif
    fuse_session_destroy(se);
#ifndef FUSE3
    fuse_unmount(gfs->gfs_mountpoint[id], gfs->gfs_ch[id]);
#endif
    return NULL;
}

/* Mount a device at the specified mount point */
static int
lc_fuseMount(struct gfs *gfs, char **arg, char *device, int argc,
             enum lc_mountId id) {
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
        goto out;
    }
    if (opts.show_version) {
        printf("FUSE library version %s\n", fuse_pkgversion());
        fuse_lowlevel_version();
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
    if (id == LC_BASE_MOUNT) {
        err = pthread_create(&gfs->gfs_mountThread, NULL,
                             lc_serve, (void *)id);
        if (err) {
            perror("pthread_create");
        } else {
            printf("%s mounted at %s\n", device, mountpoint);
        }
    } else {

        /* Wait for first thread to complete */
        pthread_mutex_lock(&gfs->gfs_lock);
        while (gfs->gfs_mcount == 0) {
            pthread_cond_wait(&gfs->gfs_mountCond, &gfs->gfs_lock);
        }
        pthread_mutex_unlock(&gfs->gfs_lock);
        if (!gfs->gfs_unmounting) {
            printf("%s mounted at %s\n", device, mountpoint);
            lc_serve((void *)id);
        } else {
            fprintf(stderr, "Aborting mount, base layer unmounted\n");
            err = EIO;
        }
    }
    mountpoint = NULL;

out:
    if (mountpoint) {
        lc_free(NULL, mountpoint, 0, LC_MEMTYPE_GFS);
    }
    fuse_opt_free_args(&args);
    return err;
}

/* Mount the specified device and start serving requests */
int
main(int argc, char *argv[]) {
    char *arg[argc + 1], completed;
    int i, err = -1, waiter[2];
    bool daemon = argc == 4;
    struct stat st;

#ifdef FUSE3
    if (argc < 4) {
#else
    if ((argc < 4) || (argc > 6)) {
#endif
        usage(argv[0]);
        exit(EINVAL);
    }

    if (!strcmp(argv[2], argv[3])) {
        fprintf(stderr, "Specify different mount points\n");
        usage(argv[0]);
        exit(EINVAL);
    }

    /* Make sure mount points exist */
    if (stat(argv[2], &st) || stat(argv[3], &st)) {
        perror("stat");
        fprintf(stderr,
                "Make sure directories %s and %s exist\n", argv[2], argv[3]);
        usage(argv[0]);
        exit(errno);
    }
    if (!daemon) {
        printf("%s %s\n", Build, Release);
    }

    /* XXX Block signals around lc_mount/lc_unmount calls */
    err = lc_mount(argv[1], &gfs);
    if (err) {
        fprintf(stderr, "Mounting %s failed, err %d\n", argv[1], err);
        exit(err);
    }

    /* Setup arguments for fuse mount */
    arg[0] = argv[0];
    arg[1] = argv[2];
    arg[2] = "-o";
    arg[3] = lc_malloc(NULL, LC_SIZEOF_MOUNTARGS, LC_MEMTYPE_GFS);
    sprintf(arg[3], "allow_other,auto_unmount,noatime,default_permissions,"
#ifndef FUSE3
                    "nonempty,atomic_o_trunc,big_writes,"
                    "splice_move,splice_read,splice_write,"
#endif
                    "subtype=lcfs,fsname=%s", argv[1]);
    for (i = 4; i < argc; i++) {
        arg[i] = argv[i];
    }

    /* Fork a new process if run in background mode */
    if (daemon) {
        err = pipe(waiter);
        if (err) {
            perror("pipe");
            goto out;
        }
        gfs->gfs_waiter = waiter;
        switch (fork()) {
        case -1:
            perror("fork");
            goto out;

        case 0:
            break;

        default:

            /* Wait for the mount to complete */
            err = read(waiter[0], &completed, sizeof(completed));
            exit(0);
        }
    }

    /* Mount the device at given mount points */
    err = lc_fuseMount(gfs, arg, argv[1], argc, LC_BASE_MOUNT);
    if (!err) {
        arg[1] = argv[3];
        err = lc_fuseMount(gfs, arg, argv[1], argc, LC_LAYER_MOUNT);
    }
    for (i = 0; i < LC_MAX_MOUNTS; i++) {
        if (gfs->gfs_mountpoint[i]) {
            lc_free(NULL, gfs->gfs_mountpoint[i], 0, LC_MEMTYPE_GFS);
        }
    }

out:
    if (!gfs->gfs_unmounting) {
        gfs->gfs_unmounting = true;
        lc_unmount(gfs);
    }
    lc_free(NULL, arg[3], LC_SIZEOF_MOUNTARGS, LC_MEMTYPE_GFS);
    lc_free(NULL, gfs, sizeof(struct gfs), LC_MEMTYPE_GFS);
    printf("%s unmounted\n", argv[1]);
    lc_displayGlobalMemStats();
    return err ? 1 : 0;
}
