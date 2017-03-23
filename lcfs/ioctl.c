#include "includes.h"
#include <sys/ioctl.h>

/* Display usage and exit */
static void
usage(char *name) {
    fprintf(stderr, "usage: %s <mnt> <id> [-c]\n", name);
    fprintf(stderr, "\t mnt   - mount point\n");
    fprintf(stderr, "\t id    - layer name, syncer time in seconds or "
            "pcache limit in MB\n");
    fprintf(stderr, "\t [-c]  - clear stats (optional)\n");
    fprintf(stderr, "Specify . as id for displaying stats for all layers\n");
    exit(EINVAL);
}

/* Display (and optionally clear) stats of a layer.
 * Issue the command from the layer root directory.
 */
int
ioctl_main(int argc, char *argv[]) {
    int fd, err, len, value;
    char name[256], *dir;
    struct stat st;

    if ((argc != 2) && (argc != 3) && (argc != 4)) {
        usage(argv[0]);
    }
    if (stat(argv[1], &st)) {
        perror("stat");
        fprintf(stderr, "Make sure %s exists\n", argv[1]);
        usage(argv[0]);
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
            usage(argv[0]);
        }
        if ((argc == 4) && strcmp(argv[3], "-c")) {
            usage(argv[0]);
        }
        err = ioctl(fd, _IOW(0, argc == 3 ? LAYER_STAT : CLEAR_STAT, name),
                    argv[2]);
    } else if (strcmp(argv[0], "flush") == 0) {
        if (argc != 2) {
            usage(argv[0]);
        }
        err = ioctl(fd, _IO(0, DCACHE_FLUSH), 0);
    } else {
        if (argc < 3) {
            usage(argv[0]);
        }
        value = atoll(argv[2]);
        if ((value < 0) || (argc != 3)) {
            close(fd);
            usage(argv[0]);
        }
        openlog("lcfs", LOG_PID|LOG_CONS|LOG_PERROR, LOG_USER);
        if (strcmp(argv[0], "syncer") == 0) {
            err = ioctl(fd, _IOW(0, SYNCER_TIME, value), argv[2]);
        } else if (value && (strcmp(argv[0], "pcache") == 0)) {
            err = ioctl(fd, _IOW(0, DCACHE_MEMORY, value), argv[2]);
        } else {
            close(fd);
            closelog();
            usage(argv[0]);
        }
        closelog();
    }
    if (err) {
        perror("ioctl");
        close(fd);
        exit(errno);
    }
    close(fd);
    return 0;
}
