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
lc_loop(struct fuse_session *se, int foreground) {
    int err = -1;

    if (fuse_set_signal_handlers(se) != -1) {
        fuse_session_add_chan(se, gfs->gfs_ch);
        fuse_daemonize(foreground);
        err = fuse_session_loop_mt(se);
        fuse_remove_signal_handlers(se);
        fuse_session_remove_chan(gfs->gfs_ch);
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
    int i, err = -1, foreground;
    char *mountpoint = NULL;
    struct fuse_session *se;
    char *arg[argc + 1];

    if ((argc < 3) || (argc > 5)) {
        usage(argv[0]);
        exit(EINVAL);
    }
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
            "subtype=lcfs,fsname=%s,big_writes,noatime,"
            "splice_move,splice_read,splice_write",
            argv[1]);
    for (i = 3; i < argc; i++) {
        arg[i + 1] = argv[i];
    }

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
    lc_displayGlobalMemStats();
    return err ? 1 : 0;
}
