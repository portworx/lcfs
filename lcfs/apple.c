#include "includes.h"

/* Open a device */
int
lc_deviceOpen(char *device) {
    int fd;

    fd = open(device, O_RDWR | O_DIRECT | O_EXCL | O_NOATIME, 0);
    if (fd != -1) {
        err = fcntl(fd, F_NOCACHE);
        if (err == -1) {
            perror("fcntl");
            close(fd);
            return -1;
        }
    }
    return fd;
}

/* Find out how much memory the system has */
uint64_t
lc_getTotalMemory() {
    size_t usermemlen = 8;
    uint8_t usermembuf[usermemlen];
    int mib[2];

    mib[0] = CTL_HW;
    mib[1] = HW_USERMEM;
    sysctl(mib, 2, usermembuf, &usermemlen, NULL, 0);
    return (usermemlen == sizeof(uint64_t)) ?
                *(uint64_t *)usermembuf : *(uint32_t *)usermembuf;
}
