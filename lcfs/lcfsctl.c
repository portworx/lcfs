#include "includes.h"
#include <sys/ioctl.h>
#include "version/version.h"

/* Display usage and exit */
static void
usage(char *name) {
    fprintf(stderr, "usage: %s <mnt> <cmd> <id> [-c]\n", name);
    fprintf(stderr, "\t mnt   - mount point\n");
    fprintf(stderr, "\t cmd   - cmd - stats, syncer or pcache \n");
    fprintf(stderr, "\t id    - layer name, syncer time in seconds or"
            "pcache limit in MB\n");
    fprintf(stderr, "\t [-c]  - clear stats (optional)\n");
    fprintf(stderr, "Specify . as id for displaying stats for all layers\n");
    exit(EINVAL);
}

/* Display (and optionally clear) stats of a layer.
 * Issue the command from the layer root directory.
 */
int
main(int argc, char *argv[]) {
    int fd, err, len, value;
    char name[256], *dir;
    struct stat st;

    if ((argc != 4) && (argc != 5)) {
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
        fprintf(stderr, "Make sure %s exists\n", dir);
        exit(errno);
    }
    if (strcmp(argv[2], "stats") == 0) {
        if ((argc == 5) && strcmp(argv[4], "-c")) {
            usage(argv[0]);
        }
        err = ioctl(fd, _IOW(0, argc == 4 ? LAYER_STAT : CLEAR_STAT, name),
                    argv[3]);
    } else {
        value = atoll(argv[3]);
        if ((value < 0) || (argc != 4)) {
            close(fd);
            usage(argv[0]);
        }
        if (strcmp(argv[2], "syncer") == 0) {
            err = ioctl(fd, _IOW(0, SYNCER_TIME, value), argv[3]);
        } else if (value && (strcmp(argv[2], "pcache") == 0)) {
            err = ioctl(fd, _IOW(0, DCACHE_MEMORY, value), argv[3]);
        } else {
            close(fd);
            usage(argv[0]);
        }
    }
    if (err) {
        perror("ioctl");
        close(fd);
        exit(errno);
    } else {
        printf("%s %s\n", Build, Release);
    }
    close(fd);
    return 0;
}
