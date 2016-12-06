#include "includes.h"
#include <sys/ioctl.h>

/* Display usage and exit */
static void
usage(char *name) {
    printf("usage: %s <id> [-c]\n", name);
    printf("Specify . as id for displaying stats for all layers\n");
    printf("Run this command from the layer root directory\n");
    exit(EINVAL);
}

/* Display (and optionally clear) stats of a layer.
 * Issue the command from the layer root directory.
 */
int
main(int argc, char *argv[]) {
    char name[256];
    int fd, err;

    if ((argc != 2) && (argc != 3)) {
        usage(argv[0]);
    }
    if ((argc == 3) && strcmp(argv[2], "-c")) {
        usage(argv[0]);
    }
    fd = open(".", O_DIRECTORY);
    if (fd < 0) {
        perror("open");
        exit(errno);
    }
    err = ioctl(fd, _IOW(0, argc == 2 ? SNAP_STAT : CLEAR_STAT, name),
                argv[1]);
    if (err) {
        perror("ioctl");
        exit(errno);
    }
    return 0;
}
