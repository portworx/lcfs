#include "includes.h"
#include <sys/ioctl.h>

#define LAYER_NAME_MAX  255

/* Display usage and exit */
static void
usage(char *pgm, char *name) {
    if (strcmp(name, "stats") == 0) {
        fprintf(stderr, "usage: %s %s <mnt> <id> [-c]\n", pgm, name);
        fprintf(stderr, "\t mnt    - mount point\n");
        fprintf(stderr, "\t id     - layer name\n");
        fprintf(stderr, "\t [-c]   - clear stats (optional)\n");
        fprintf(stderr,
                "Specify . as id for displaying stats for all layers\n");
    } else if (strcmp(name, "syncer") == 0) {
        fprintf(stderr, "usage: %s %s <mnt> <time>\n", pgm, name);
        fprintf(stderr, "\t mnt    - mount point\n");
        fprintf(stderr, "\t time   - time in seconds, "
                "0 to disable (default 1 minute)\n");
    } else if (strcmp(name, "pcache") == 0) {
        fprintf(stderr, "usage: %s %s <mnt> <pcache>\n", pgm, name);
        fprintf(stderr, "\t mnt    - mount point\n");
        fprintf(stderr, "\t memory - memory limit in MB (default 512MB)\n");
#ifndef __MUSL__
    } else if (strcmp(name, "profile") == 0) {
        fprintf(stderr, "usage: %s %s <mnt> [enable|disable]\n", pgm, name);
        fprintf(stderr, "\t mnt              - mount point\n");
        fprintf(stderr, "\t [enable|disable] - enable/disable profiling\n");
#endif
    } else if (strcmp(name, "verbose") == 0) {
        fprintf(stderr, "usage: %s %s <mnt> [enable|disable]\n", pgm, name);
        fprintf(stderr, "\t mnt              - mount point\n");
        fprintf(stderr, "\t [enable|disable] - enable/disable verbose mode\n");
    } else {
        fprintf(stderr, "usage: %s %s <mnt>\n", pgm, name);
        fprintf(stderr, "\t mnt    - mount point\n");
    }
    exit(EINVAL);
}

/* Display (and optionally clear) stats of a layer.
 * Issue the command from the layer root directory.
 */
int
ioctl_main(char *pgm, int argc, char *argv[]) {
    char name[LAYER_NAME_MAX + 1], *dir, op;
    int fd, err, len, value;
    enum ioctl_cmd cmd;
    struct stat st;

    if ((argc != 2) && (argc != 3) && (argc != 4)) {
        usage(pgm, argv[0]);
    }
    if (stat(argv[1], &st)) {
        perror("stat");
        fprintf(stderr, "Make sure %s exists\n", argv[1]);
        usage(pgm, argv[0]);
    }
    len = strlen(argv[1]);
    dir = alloca(len + strlen(LC_LAYER_ROOT_DIR) + 3);
    memcpy(dir, argv[1], len);
    dir[len] = '/';
    memcpy(&dir[len + 1], LC_LAYER_ROOT_DIR, strlen(LC_LAYER_ROOT_DIR));
    len += strlen(LC_LAYER_ROOT_DIR) + 1;
    dir[len] = 0;
    fd = open(dir, O_DIRECTORY);
    if (fd < 0) {
        perror("open");
        fprintf(stderr, "Make sure %s exists and has permissions\n", dir);
        exit(errno);
    }
    if (strcmp(argv[0], "stats") == 0) {
        if (argc < 3) {
            close(fd);
            usage(pgm, argv[0]);
        }
        if ((argc == 4) && strcmp(argv[3], "-c")) {
            close(fd);
            usage(pgm, argv[0]);
        }
        len = strlen(argv[2]);
        assert(len < LAYER_NAME_MAX);
        memcpy(name, argv[2], len);
        name[len] = 0;
        cmd = (argc == 3) ? LAYER_STAT : CLEAR_STAT;
        err = ioctl(fd, _IOW(0, cmd, name), name);
    } else if (strcmp(argv[0], "flush") == 0) {
        if (argc != 2) {
            close(fd);
            usage(pgm, argv[0]);
        }
        err = ioctl(fd, _IO(0, DCACHE_FLUSH), 0);
    } else if (strcmp(argv[0], "grow") == 0) {
        if (argc != 2) {
            close(fd);
            usage(pgm, argv[0]);
        }
        err = ioctl(fd, _IO(0, LCFS_GROW), 0);
    } else if (strcmp(argv[0], "commit") == 0) {
        if (argc != 2) {
            close(fd);
            usage(pgm, argv[0]);
        }
        err = ioctl(fd, _IO(0, LCFS_COMMIT), 0);
    } else if ((strcmp(argv[0], "verbose") == 0)
#ifndef __MUSL__
               || (strcmp(argv[0], "profile") == 0)
#endif
               ) {
        if (argc < 3) {
            close(fd);
            usage(pgm, argv[0]);
        }
        if (strcmp(argv[2], "enable") == 0) {
            op = 1;
        } else if (strcmp(argv[2], "disable") == 0) {
            op = 0;
        } else {
            close(fd);
            usage(pgm, argv[0]);
        }
        cmd =
#ifndef __MUSL__
            (strcmp(argv[0], "profile") == 0) ? LCFS_PROFILE :
#endif
            LCFS_VERBOSE;
        err = ioctl(fd, _IOW(0, cmd, op), &op);
    } else {
        if (argc != 3) {
            close(fd);
            usage(pgm, argv[0]);
        }
        value = atoll(argv[2]);
        if (value < 0) {
            close(fd);
            usage(pgm, argv[0]);
        }
        if (strcmp(argv[0], "syncer") == 0) {
            err = ioctl(fd, _IOW(0, SYNCER_TIME, int), argv[2]);
        } else if (value && (strcmp(argv[0], "pcache") == 0)) {
            err = ioctl(fd, _IOW(0, DCACHE_MEMORY, int), argv[2]);
        } else {
            close(fd);
            usage(pgm, argv[0]);
        }
    }
    if (err) {
        perror("ioctl");
        close(fd);
        exit(errno);
    }
    close(fd);
    return 0;
}
