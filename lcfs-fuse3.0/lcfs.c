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
lc_loop(struct fuse_session *se, char *mountpoint, int foreground) {
    int err = -1;

    fuse_set_signal_handlers(se);
    fuse_session_mount(se, mountpoint);
    fuse_daemonize(foreground);
    /* XXX Experiment with clone fd argument */
    err = fuse_session_loop_mt(se, 0);
    fuse_remove_signal_handlers(se);
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
            err = lc_mount("/dev/sdb", &gfs);
            if (err) {
                printf("Mounting %s failed, err %d\n", argv[1], err);
            } else if ((se = fuse_session_new(&args, &lc_ll_oper,
                                       sizeof(lc_ll_oper), gfs))) {
                printf("%s mounted at %s\n", argv[1], opts.mountpoint);
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
    lc_displayGlobalMemStats();
    return err ? 1 : 0;
}
