#include "includes.h"
#include <sys/ioctl.h>

int
main(int argc, char *argv[]) {
    char name[256];
    int fd, err;

    if ((argc != 2) && (argc != 3)) {
        printf("usage: %s <id> [-c]\n", argv[0]);
        exit(EINVAL);
    }
    if ((argc == 3) && strcmp(argv[2], "-c")) {
        printf("usage: %s <id> [-c]\n", argv[0]);
        exit(EINVAL);
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
