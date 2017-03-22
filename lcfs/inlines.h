#ifndef _INLINES_H
#define _INLINES_H

#define lc_syslog(level, ...)  syslog(level, __VA_ARGS__)

#if 0
#define lc_printf(...)  lc_syslog(LOG_INFO,  __VA_ARGS__)
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

/* Update a value atomically */
static inline void
lc_atomicUpdate(struct fs *fs, uint64_t *value, uint64_t change, bool incr) {
    uint64_t decr;

    if (incr) {
        if (fs->fs_locked) {
            *value = *value + change;
        } else {
            __sync_add_and_fetch(value, change);
        }
    } else {
        if (fs->fs_locked) {
            assert(change <= *value);
            *value = *value - change;
        } else {
            decr = __sync_fetch_and_sub(value, change);
            assert(change <= decr);
        }
    }
}

#endif
