#ifndef _LINUX_H_
#define _LINUX_H_

/* Return requested time from stat structure */
static inline struct timespec
lc_statGetTime(struct stat *attr, bool mtime) {
    return mtime ? attr->st_mtim : attr->st_ctim;
}

/* Copy times from disk inode to stat structure */
static inline void
lc_copyStatTimes(struct stat *st, struct dinode *dinode) {

    /* atime is not tracked */
    st->st_atim = dinode->di_mtime;
    st->st_mtim = dinode->di_mtime;
    st->st_ctim = dinode->di_ctime;
}

/* Get current time */
static inline void
lc_gettime(struct timespec *tv) {
    clock_gettime(CLOCK_REALTIME, tv);
}

/* Invoke pwritev(2) */
static inline ssize_t
lc_pwritev(int fd, struct iovec *iov, int iovcnt, off_t offset) {
    return pwritev(fd, iov, iovcnt, offset);
}

/* Invoke preadv(2) */
static inline ssize_t
lc_preadv(int fd, struct iovec *iov, int iovcnt, off_t offset) {
  return preadv(fd, iov, iovcnt, offset);
}

/* Validate a lock is held */
static inline void
lc_lockOwned(pthread_rwlock_t *lock, bool exclusive) {
    assert((lock == NULL) || lock->__data.__writer ||
           (!exclusive && lock->__data.__nr_readers));
}

#endif
