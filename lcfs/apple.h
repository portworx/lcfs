#ifndef _APPLE_H_
#define _APPLE_H_

#define _IOC_NRBITS     8
#define _IOC_TYPEBITS   8
#define _IOC_NRMASK     ((1 << _IOC_NRBITS)-1)
#define _IOC_TYPEMASK   ((1 << _IOC_TYPEBITS)-1)
#define _IOC_NRSHIFT    0
#define _IOC_TYPESHIFT  (_IOC_NRSHIFT+_IOC_NRBITS)
#define _IOC_TYPE(nr)           (((nr) >> _IOC_TYPESHIFT) & _IOC_TYPEMASK)
#define _IOC_NR(nr)             (((nr) >> _IOC_NRSHIFT) & _IOC_NRMASK)

/* Return requested time from stat structure */
static inline struct timespec
lc_statGetTime(struct stat *attr, bool mtime) {
    return mtime ? attr->st_mtimespec : attr->st_ctimespec;
}

/* Copy times from disk inode to stat structure */
static inline void
lc_copyStatTimes(struct stat *st, struct dinode *dinode) {

    /* atime is not tracked */
    st->st_atimespec = dinode->di_mtime;
    st->st_mtimespec = dinode->di_mtime;
    st->st_ctimespec = dinode->di_ctime;
}

/* Get current time */
static inline void
lc_gettime(struct timespec *tv) {
    clock_serv_t cclock;
    mach_timespec_t mach_ts;

    host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
    clock_get_time(cclock, &mach_ts);
    mach_port_deallocate(mach_task_self(), cclock);
    tv->tv_sec = mach_ts.tv_sec;
    tv->tv_nsec = mach_ts.tv_nsec;
}

/* Implement pwritev equivalent using writev */
static inline ssize_t
lc_pwritev(int fd, struct iovec *iov, int iovcnt, off_t offset) {
    ssize_t count = 0;

    for (int i = 0; i < iovcnt; i++) {
        assert(iov[i].iov_len == LC_BLOCK_SIZE);
        count += pwrite(fd, iov[i].iov_base, LC_BLOCK_SIZE, offset);
        offset += LC_BLOCK_SIZE;
    }
    return count;
}

/*Implement preadv equivalent using readv */
static inline ssize_t
lc_preadv(int fd, struct iovec *iov, int iovcnt, off_t offset) {
    ssize_t count = 0;

    for (int i = 0; i < iovcnt; i++) {
      assert(iov[i].iov_len == LC_BLOCK_SIZE);
      count += pread(fd, iov[i].iov_base, LC_BLOCK_SIZE, offset);
      offset += LC_BLOCK_SIZE;
    }
    return count;
}

#endif
