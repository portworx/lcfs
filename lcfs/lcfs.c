#include "includes.h"

static struct gfs *gfs;
extern struct fuse_lowlevel_ops lc_ll_oper;

#define LC_SIZEOF_MOUNTARGS 1024

/* XXX Figure out a better way to find userdata with low level fuse */
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
#endif
        int foreground) {
    pthread_t flusher;
    int err = -1;

#ifdef FUSE3
    fuse_set_signal_handlers(se);
    fuse_session_mount(se, mountpoint);
#else
    if (fuse_set_signal_handlers(se) != -1) {
        fuse_session_add_chan(se, gfs->gfs_ch);
#endif
        fuse_daemonize(foreground);
        err = pthread_create(&flusher, NULL, lc_flusher, NULL);
        if (!err) {
            /* XXX Experiment with clone fd argument */
            err = fuse_session_loop_mt(se
#ifdef FUSE3
                                       , 0
#endif
                                       );
            fuse_remove_signal_handlers(se);
            pthread_cancel(flusher);
            pthread_join(flusher, NULL);
#ifndef FUSE3
            fuse_session_remove_chan(gfs->gfs_ch);
        }
#endif
    }
    return err;
}

/* Display usage */
static void
usage(char *prog) {
    printf("usage: %s <device> <mnt> [-d] [-f]\n", prog);
}

/* Mount the specified device and start serving requests */
int
main(int argc, char *argv[]) {
#ifdef FUSE3
    struct fuse_cmdline_opts opts;
    struct fuse_session *se;
    int i, err = -1, ret;
    char *arg[argc + 1];

    if (argc < 3) {
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
            err = lc_mount("/dev/sdb", &gfs);
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
    int i, err = -1, foreground;
    char *mountpoint = NULL;
    struct fuse_session *se;
    char *arg[argc + 1];

    if ((argc < 3) || (argc > 5)) {
        usage(argv[0]);
        exit(EINVAL);
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
    for (i = 3; i < argc; i++) {
        arg[i + 1] = argv[i];
    }
    lc_memoryInit();

    struct fuse_args args = FUSE_ARGS_INIT(argc + 1, arg);
    if ((fuse_parse_cmdline(&args, &mountpoint, NULL, &foreground) != -1) &&
        ((gfs->gfs_ch = fuse_mount(mountpoint, &args)) != NULL)) {
        se = fuse_lowlevel_new(&args, &lc_ll_oper,
                               sizeof(lc_ll_oper), gfs);
        lc_free(NULL, arg[3], LC_SIZEOF_MOUNTARGS, LC_MEMTYPE_GFS);
        if (se) {
            printf("%s mounted at %s\n", argv[1], mountpoint);
            err = lc_loop(se, foreground);
            fuse_session_destroy(se);
        } else {
            err = EINVAL;
            usage(argv[0]);
        }
        fuse_unmount(mountpoint, gfs->gfs_ch);
    } else {
        lc_free(NULL, arg[3], LC_SIZEOF_MOUNTARGS, LC_MEMTYPE_GFS);
        usage(argv[0]);
        err = EINVAL;
        lc_unmount(gfs);
    }
    fuse_opt_free_args(&args);
    if (mountpoint) {
        lc_free(NULL, mountpoint, 0, LC_MEMTYPE_GFS);
    }
    lc_free(NULL, gfs, sizeof(struct gfs), LC_MEMTYPE_GFS);
    printf("%s unmounted\n", argv[1]);
#endif
    lc_displayGlobalMemStats();
    return err ? 1 : 0;
}
