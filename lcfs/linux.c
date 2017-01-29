#include "includes.h"

/* Open a device */
int
lc_deviceOpen(char *device) {
    return open(device, O_RDWR | O_DIRECT | O_EXCL | O_NOATIME, 0);
}

/* Find out how much memory the system has */
uint64_t
lc_getTotalMemory() {
    struct sysinfo info;

    sysinfo(&info);
    return info.totalram;
}
