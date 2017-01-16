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
    printf("usage: %s <device> <mnt> <mnt2> [-d]\n", prog);
    printf("\tdevice - device/file\n"
           "\tmnt    - mount point on host\n"
           "\tmnt2   - mount point propagated to plugin\n"
           "\t-d     - display debugging info (optional)\n");
    printf("%s\n", Build);
    printf("%s\n", Release);
}

/* Data passed to the duplicate thread */
struct fuseData {

    /* Fuse session */
    struct fuse_session *fd_se;

#ifndef FUSE3
    /* Fuse channel */
    struct fuse_chan *fd_ch;
#endif

    /* Mount point */
    char *fd_mountpoint;

    /* Global file system */
    struct gfs *fd_gfs;

    /* Set if running as a thread */
    bool fd_thread;
};

static struct fuseData fd[LC_MAX_MOUNTS];

/* Serve file system requests */
static void *
lc_serve(void *data) {
    struct fuseData *fd = (struct fuseData *)data;
    struct gfs *gfs = fd->fd_gfs;
    struct fuse_session *se;
    bool fcancel = false;
    pthread_t flusher;
    int err;

    if (!fd->fd_thread) {
        if (fuse_set_signal_handlers(fd->fd_se) == -1) {
            printf("Error setting signal handlers\n");
            err = EPERM;
            goto out;
        }
        err = pthread_create(&flusher, NULL, lc_flusher, NULL);
        if (err) {
            printf("Flusher thread could not be created, err %d\n", err);
            goto out;
        }
        fcancel = true;
    }
#ifdef FUSE3
    fuse_session_mount(fd->fd_se, fd->fd_mountpoint);
#else
    fuse_session_add_chan(fd->fd_se, fd->fd_ch);
#endif
    err = fuse_session_loop_mt(fd->fd_se
#ifdef FUSE3
    /* XXX Experiment with clone fd argument */
                               , 0);
#else
                               );
    fuse_session_remove_chan(fd->fd_ch);
#endif

out:
    gfs->gfs_unmounting = true;

    /* Other mount need to exit as well */
    pthread_mutex_lock(&gfs->gfs_lock);
    se = gfs->gfs_se[fd->fd_thread ? LC_LAYER_MOUNT : LC_BASE_MOUNT];
    if (se) {
        fuse_session_exit(se);
    }
    if (fd->fd_thread) {
        gfs->gfs_se[LC_BASE_MOUNT] = NULL;
        pthread_mutex_unlock(&gfs->gfs_lock);
    } else {
        gfs->gfs_se[LC_LAYER_MOUNT] = NULL;
        pthread_mutex_unlock(&gfs->gfs_lock);
        fuse_remove_signal_handlers(fd->fd_se);

        /* Wait for flusher thread to exit */
        if (fcancel) {
            pthread_mutex_lock(&gfs->gfs_lock);
            pthread_cond_broadcast(&gfs->gfs_flusherCond);
            pthread_mutex_unlock(&gfs->gfs_lock);
            pthread_join(flusher, NULL);
        }
    }
#ifdef FUSE3
    fuse_session_unmount(fd->fd_se);
#endif
    fuse_session_destroy(fd->fd_se);
#ifndef FUSE3
    fuse_unmount(fd->fd_mountpoint, fd->fd_ch);
#endif
    lc_free(NULL, fd->fd_mountpoint, 0, LC_MEMTYPE_GFS);
    return NULL;
}

/* Mount a device at the specified mount point */
static int
lc_fuseMount(struct gfs *gfs, char **arg, char *device, int argc,
             bool thread) {
    enum lc_mountId id = thread ? LC_BASE_MOUNT : LC_LAYER_MOUNT;
    struct fuse_args args = FUSE_ARGS_INIT(argc, arg);
    struct fuse_session *se;
    char *mountpoint = NULL;
    pthread_t dup;
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
    struct fuse_chan *ch = NULL;

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
    fd[id].fd_ch = ch;
    se = fuse_lowlevel_new
#endif
                          (&args, &lc_ll_oper, sizeof(lc_ll_oper),
                           id ? gfs : NULL);
    if (!se) {
        err = EINVAL;
        goto out;
    }
    gfs->gfs_se[id] = se;
#ifndef FUSE3
    if (id == 1) {
        gfs->gfs_ch = ch;
    }
#endif
    fd[id].fd_gfs = gfs;
    fd[id].fd_se = se;
    fd[id].fd_thread = thread;
    fd[id].fd_mountpoint = mountpoint;
    if (thread) {
        err = pthread_create(&dup, NULL, lc_serve, &fd[id]);
        if (!err) {
            printf("%s mounted at %s\n", device, mountpoint);
        }
    } else {
        printf("%s mounted at %s\n", device, mountpoint);
        lc_serve(&fd[id]);
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
    char *arg[argc + 1];
    int i, err = -1;
    struct stat st;

#ifdef FUSE3
    if (argc < 4) {
#else
    if ((argc < 4) || (argc > 5)) {
#endif
        usage(argv[0]);
        exit(EINVAL);
    }

    if (!strcmp(argv[2], argv[3])) {
        printf("Specify different mount points\n");
        usage(argv[0]);
        exit(EINVAL);
    }

    /* Make sure mount points exist */
    if (stat(argv[2], &st) || stat(argv[3], &st)) {
        perror("stat");
        printf("Make sure directories %s and %s exist\n", argv[2], argv[3]);
        usage(argv[0]);
        exit(errno);
    }

    /* XXX Block signals around lc_mount/lc_unmount calls */
    err = lc_mount(argv[1], &gfs);
    if (err) {
        printf("Mounting %s failed, err %d\n", argv[1], err);
        exit(err);
    }

    /* Setup arguments for fuse mount */
    arg[0] = argv[0];
    arg[1] = argv[2];
    arg[2] = "-o";
    arg[3] = lc_malloc(NULL, LC_SIZEOF_MOUNTARGS, LC_MEMTYPE_GFS);
    sprintf(arg[3], "allow_other,auto_unmount,noatime,subtype=lcfs,fsname=%s,"
#ifndef FUSE3
            "nonempty,atomic_o_trunc,big_writes,"
            "splice_move,splice_read,splice_write,"
#endif
            "default_permissions", argv[1]);
    for (i = 4; i < argc; i++) {
        arg[i] = argv[i];
    }

    /* Mount the device at given mount points */
    err = lc_fuseMount(gfs, arg, argv[1], argc, true);
    if (!err) {
        arg[1] = argv[3];
        lc_fuseMount(gfs, arg, argv[1], argc, false);
    }
    lc_free(NULL, arg[3], LC_SIZEOF_MOUNTARGS, LC_MEMTYPE_GFS);
    lc_free(NULL, gfs, sizeof(struct gfs), LC_MEMTYPE_GFS);
    printf("%s unmounted\n", argv[1]);
    lc_displayGlobalMemStats();
    return err ? 1 : 0;
}
