#include "includes.h"

static struct gfs *gfs;
extern struct fuse_lowlevel_ops lc_ll_oper;

#define LC_SIZEOF_MOUNTARGS 1024

/* Return global file system */
struct gfs *
getfs() {
    return gfs;
}

/* Process fuse requests */
static int
lc_loop(struct fuse_session *se,
#ifdef FUSE3
        char *mountpoint,
#else
        struct fuse_chan *ch,
#endif
        int foreground) {
    int err = -1;

#ifdef FUSE3
    fuse_set_signal_handlers(se);
    fuse_session_mount(se, mountpoint);
#else
    if (fuse_set_signal_handlers(se) != -1) {
        fuse_session_add_chan(se, ch);
#endif
        fuse_daemonize(foreground);
        err = fuse_session_loop_mt(se
#ifdef FUSE3
        /* XXX Experiment with clone fd argument */
                                   , 0
#endif
                                   );
        fuse_remove_signal_handlers(se);
#ifndef FUSE3
        fuse_session_remove_chan(ch);
    }
#endif
    return err;
}

/* Display usage */
static void
usage(char *prog) {
    printf("usage: %s <device> <mnt> <mnt2> [-d] [-f]\n", prog);
}

/* Data passed to the duplicate thread */
struct fuseData {

    /* Fuse session */
    struct fuse_session *fd_se;

    /* Fuse channel */
    struct fuse_chan *fd_ch;

    /* Mount point */
    char *fd_mountpoint;

    /* Run in foreground or not */
    int fd_foreground;
};

static struct fuseData fd;

/* Thread for starting services on duplicate mountpoint */
static void *
lc_dup(void *data) {
    struct fuseData *fd = (struct fuseData *)data;

    lc_loop(fd->fd_se, fd->fd_ch, fd->fd_foreground);
    fuse_session_destroy(fd->fd_se);
    fuse_unmount(fd->fd_mountpoint, fd->fd_ch);
    lc_free(NULL, fd->fd_mountpoint, 0, LC_MEMTYPE_GFS);
    return NULL;
}

/* Mount a device at the specified mount point */
static int
lc_fuseMount(struct gfs *gfs, char **arg, char *device, int argc, bool first) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, arg);
    struct fuse_chan *ch = NULL;
    char *mountpoint = NULL;
    struct fuse_session *se;
    int err, foreground;
    pthread_t flusher;

    err = fuse_parse_cmdline(&args, &mountpoint, NULL, &foreground);
    if (err == -1) {
        err = EINVAL;
        goto out;
    }
    ch = fuse_mount(mountpoint, &args);
    if (!ch) {
        err = EINVAL;
        goto out;
    }
    se = fuse_lowlevel_new(&args, &lc_ll_oper, sizeof(lc_ll_oper), gfs);
    if (!se) {
        err = EINVAL;
        goto out;
    }
    printf("%s mounted at %s\n", device, mountpoint);
    if (first) {
        fd.fd_se = se;
        fd.fd_ch = ch;
        fd.fd_foreground = foreground;
        fd.fd_mountpoint = mountpoint;
        err = pthread_create(&gfs->gfs_dup, NULL, lc_dup, &fd);
        mountpoint = NULL;
        ch = NULL;
    } else {
        gfs->gfs_ch = ch;
        err = pthread_create(&flusher, NULL, lc_flusher, NULL);
        if (!err) {
            if (!err) {
                err = lc_loop(se, ch, foreground);
            }
            pthread_cancel(flusher);
            pthread_join(flusher, NULL);
        }
        if (gfs->gfs_dup) {
            pthread_cancel(gfs->gfs_dup);
            pthread_join(gfs->gfs_dup, NULL);
        }
        fuse_session_destroy(se);
        fuse_unmount(mountpoint, ch);
    }

out:
    fuse_opt_free_args(&args);
    if (mountpoint) {
        lc_free(NULL, mountpoint, 0, LC_MEMTYPE_GFS);
    }
    return err;
}

/* Mount the specified device and start serving requests */
int
main(int argc, char *argv[]) {
#ifdef FUSE3

    /* XXX Make V2 plugins work with FUSE 3.0 */
    struct fuse_cmdline_opts opts;
    struct fuse_session *se;
    int i, err = -1, ret;
    char *arg[argc];

    if (argc < 4) {
        usage(argv[0]);
        exit(EINVAL);
    }
    arg[0] = argv[0];
    arg[1] = argv[2];
    arg[2] = "-o";
    arg[3] = lc_malloc(NULL, LC_SIZEOF_MOUNTARGS, LC_MEMTYPE_GFS);
    sprintf(arg[3], "allow_other,auto_unmount,noatime,subtype=lcfs,fsname=%s",
            argv[1]);
    for (i = 3; i < argc; i++) {
        arg[i + 1] = argv[i];
    }

    struct fuse_args args = FUSE_ARGS_INIT(argc + 1, arg);
    ret = fuse_parse_cmdline(&args, &opts);
    lc_free(NULL, arg[3], LC_SIZEOF_MOUNTARGS, LC_MEMTYPE_GFS);
    if (ret != -1) {
        if (opts.show_help) {
            usage(argv[0]);
            fuse_cmdline_help();
            //fuse_lowlevel_help();
        } else if (opts.show_version) {
            printf("FUSE library version %s\n", fuse_pkgversion());
            fuse_lowlevel_version();
        } else {

            /* XXX Block signals around lc_mount/lc_unmount calls */
            err = lc_mount(argv[1], &gfs);
            if (err) {
                printf("Mounting %s failed, err %d\n", argv[1], err);
            } else if ((se = fuse_session_new(&args, &lc_ll_oper,
                                       sizeof(lc_ll_oper), gfs))) {
                printf("%s mounted at %s\n", argv[1], opts.mountpoint);
                gfs->gfs_se = se;
                err = lc_loop(se, opts.mountpoint, opts.foreground);
                fuse_session_unmount(se);
                fuse_session_destroy(se);
                printf("%s unmounted\n", argv[1]);
            } else {
                lc_unmount(gfs);
            }
            lc_free(NULL, gfs, sizeof(struct gfs), LC_MEMTYPE_GFS);
        }
    } else {
        usage(argv[0]);
        fuse_cmdline_help();
        //fuse_lowlevel_help();
        err = EINVAL;
    }
    fuse_opt_free_args(&args);
    if (opts.mountpoint) {
        lc_free(NULL, opts.mountpoint, 0, LC_MEMTYPE_GFS);
    }
#else
    char *arg[argc + 1];
    int i, err = -1;
    struct stat st;

    if ((argc < 4) || (argc > 6)) {
        usage(argv[0]);
        exit(EINVAL);
    }

    /* Make sure mount points exist */
    if (stat(argv[2], &st) || stat(argv[3], &st)) {
        perror("stat");
        usage(argv[0]);
        exit(errno);
    }

    /* XXX Block signals around lc_mount/lc_unmount calls */
    err = lc_mount(argv[1], &gfs);
    if (err) {
        printf("Mounting %s failed, err %d\n", argv[1], err);
        exit(err);
    }
    arg[0] = argv[0];
    arg[1] = argv[2];
    arg[2] = "-o";
    arg[3] = lc_malloc(NULL, LC_SIZEOF_MOUNTARGS, LC_MEMTYPE_GFS);
    sprintf(arg[3],
            "nonempty,allow_other,auto_unmount,atomic_o_trunc,"
            "subtype=lcfs,fsname=%s,big_writes,noatime,default_permissions,"
            "splice_move,splice_read,splice_write",
            argv[1]);
    for (i = 4; i < argc; i++) {
        arg[i] = argv[i];
    }

    /* Mount the device at the specified mount points */
    err = lc_fuseMount(gfs, arg, argv[1], argc, true);
    if (!err) {
        arg[1] = argv[3];
        lc_fuseMount(gfs, arg, argv[1], argc, false);
    }
    lc_unmount(gfs);
    lc_free(NULL, arg[3], LC_SIZEOF_MOUNTARGS, LC_MEMTYPE_GFS);
    lc_free(NULL, gfs, sizeof(struct gfs), LC_MEMTYPE_GFS);
    printf("%s unmounted\n", argv[1]);
#endif
    lc_displayGlobalMemStats();
    return err ? 1 : 0;
}
