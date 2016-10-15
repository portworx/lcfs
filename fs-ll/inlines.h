#ifndef _INLINES_H
#define _INLINES_H

#if 0
#define dfs_printf  printf
#else
#define dfs_printf(a...)
#endif

/* Display debug information on every file system request */
static inline void
dfs_displayEntry(const char *func, ino_t dir, ino_t ino, const char *name) {
#if 0
    /*
    if (strstr(func, "xattr") == NULL) {
        return;
    }
    if (strstr(func, "flush") != NULL) {
        return;
    }
    if (strstr(func, "release") != NULL) {
        return;
    }
    if (strstr(func, "write") != NULL) {
        return;
    }
    */
    dfs_printf("%s: ino1 %ld ino2 %ld %s\n", func, dir, ino, name ? name : "");
#endif
}

/* Report errors reported from file system operations */
static inline void
dfs_reportError(const char *func, ino_t ino, int err) {
    printf("%s: reporting error %d for inode %ld\n", func, err, ino);
}

#endif
