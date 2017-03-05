#ifndef _INLINES_H
#define _INLINES_H

#if 1
#define lc_printf  printf
#else
#define lc_printf(a...)
#endif

#if 0
/* Display debug information on every file system request */
static inline void
lc_displayEntry(const char *func, ino_t dir, ino_t ino, const char *name) {
    /*
    if (strstr(func, "xattr") != NULL) {
        return;
    }
    if (strstr(func, "flush") != NULL) {
        return;
    }
    if (strstr(func, "release") != NULL) {
        return;
    }
    if (strstr(func, "read") != NULL) {
        return;
    }
    if (strstr(func, "write") != NULL) {
        return;
    }
    if ((name == NULL) || strstr(name, "hello") == NULL) {
        return;
    }
    */
    printf("%s: ino1 %ld (%ld gindex %ld) ino2 %ld (%ld gindex %ld) %s\n", func, dir, lc_getInodeHandle(dir), lc_getFsHandle(dir), ino, lc_getInodeHandle(ino), lc_getFsHandle(ino), name ? name : "");
}
#else
#define lc_displayEntry(a...)
#endif

#if 1
/* Report errors reported from file system operations */
static inline void
lc_reportError(const char *func, int line, ino_t ino, int err) {
    printf("%s:%d: reporting error %d for inode %ld (%ld at gindex %ld)\n", func, line, err, ino, lc_getInodeHandle(ino), lc_getFsHandle(ino));
}
#else
#define lc_reportError(a...)
#endif

/* Validate a lock is held */
static inline void
lc_lockOwned(pthread_rwlock_t *lock, bool exclusive) {
    assert((lock == NULL) || lock->__data.__writer ||
           (!exclusive && lock->__data.__nr_readers));
}

#endif
