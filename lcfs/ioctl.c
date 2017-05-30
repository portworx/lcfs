#include "includes.h"
#include <sys/ioctl.h>

#define LAYER_NAME_MAX  255

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
    char name[LAYER_NAME_MAX + 1], *dir;
    int fd, err, len, value;
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
            close(fd);
            usage(argv[0]);
        }
        if ((argc == 4) && strcmp(argv[3], "-c")) {
            close(fd);
            usage(argv[0]);
        }
        len = strlen(argv[2]);
        assert(len < LAYER_NAME_MAX);
        memcpy(name, argv[2], len);
        name[len] = 0;
        err = ioctl(fd, _IOW(0, argc == 3 ? LAYER_STAT : CLEAR_STAT, name),
                    name);
    } else if (strcmp(argv[0], "flush") == 0) {
        if (argc != 2) {
            close(fd);
            usage(argv[0]);
        }
        err = ioctl(fd, _IO(0, DCACHE_FLUSH), 0);
    } else if (strcmp(argv[0], "grow") == 0) {
        if (argc != 2) {
            close(fd);
            usage(argv[0]);
        }
        err = ioctl(fd, _IO(0, LCFS_GROW), 0);
    } else if (strcmp(argv[0], "commit") == 0) {
        if (argc != 2) {
            close(fd);
            usage(argv[0]);
        }
        err = ioctl(fd, _IO(0, LCFS_COMMIT), 0);
    } else {
        if (argc != 3) {
            close(fd);
            usage(argv[0]);
        }
        value = atoll(argv[2]);
        if (value < 0) {
            close(fd);
            usage(argv[0]);
        }
        if (strcmp(argv[0], "syncer") == 0) {
            err = ioctl(fd, _IOW(0, SYNCER_TIME, int), argv[2]);
        } else if (value && (strcmp(argv[0], "pcache") == 0)) {
            err = ioctl(fd, _IOW(0, DCACHE_MEMORY, int), argv[2]);
        } else {
            close(fd);
            usage(argv[0]);
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
