#include "includes.h"

#define ARGC    4

static struct gfs *gfs;
extern struct fuse_lowlevel_ops dfs_ll_oper;

/* XXX Figure out a better way to find userdata with low level fuse */
/* Return global file system */
struct gfs *
getfs() {
    return gfs;
}

/* Process fuse requests */
static int
dfs_loop(struct fuse_session *se, bool daemon) {
    int err = -1;
    pid_t pid;
    int fd;

    /* Detach from the terminal */
    if (daemon) {
        pid = fork();
        if (pid > 0) {
            exit(0);
        }
        setsid();
        chdir("/");
        fd = open("/dev/null", O_RDWR);
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
    }
    if (fuse_set_signal_handlers(se) != -1) {
        fuse_session_add_chan(se, gfs->gfs_ch);
        err = fuse_session_loop_mt(se);
        fuse_remove_signal_handlers(se);
        fuse_session_remove_chan(gfs->gfs_ch);
    }
    return err;
}

/* Mount the specified device and start serving requests */
int
main(int argc, char *argv[]) {
    struct fuse_session *se;
    char *mountpoint;
    char *arg[ARGC];
    int err = -1;
    bool daemon;

    if (argc != 3) {
        printf("%s: device mnt\n", argv[0]);
        exit(EINVAL);
    }
    err = dfs_mount(argv[1], &gfs);
    if (err) {
        printf("Mounting %s failed, err %d\n", argv[1], err);
        exit(err);
    }
    arg[0] = argv[0];
    arg[1] = argv[2];
    arg[2] = "-o";
    arg[3] = malloc(1024);
    sprintf(arg[3],
            "allow_other,auto_unmount,atomic_o_trunc,"
            "subtype=dfs,fsname=%s,big_writes,noatime,"
            "splice_move,splice_read,splice_write",
            argv[1]);
    if (ARGC >= 5) {
        arg[4] = "-d";
        daemon = false;
    } else {
        /* XXX Add command line argument for disabling this */
        daemon = true;
    }

    struct fuse_args args = FUSE_ARGS_INIT(ARGC, arg);
    if (fuse_parse_cmdline(&args, &mountpoint, NULL, NULL) != -1 &&
        (gfs->gfs_ch = fuse_mount(mountpoint, &args)) != NULL) {
        se = fuse_lowlevel_new(&args, &dfs_ll_oper,
                               sizeof(dfs_ll_oper), gfs);
        if (se) {
            err = dfs_loop(se, daemon);
            fuse_session_destroy(se);
        }
        fuse_unmount(mountpoint, gfs->gfs_ch);
    }
    fuse_opt_free_args(&args);
    return err ? 1 : 0;
}
