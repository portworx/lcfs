#ifndef _INLINES_H
#define _INLINES_H

#if 0
#define dfs_printf  printf
#else
#define dfs_printf(a...)
#endif

/* Display debug information on every file system request */
static inline void
dfs_displayEntry(const char *func, const char *path, ino_t ino) {
    /*
    if (strstr(func, "xattr") == NULL) {
        return;
    }
    if ((path == NULL) || (strstr(path, "local-kv.db") == NULL)) {
        return;
    }
    if (ino) {
        dfs_printf("%s: path %s, ino %ld\n", func, path, ino);
    } else {
        dfs_printf("%s: path %s\n", func, path);
    }
    */
}

/* Report errors reported from file system operations */
static inline void
dfs_reportError(const char *func, const char *path, ino_t ino, int err) {
    if (ino) {
        dfs_printf("%s: reporting error %d for path %s, ino %ld\n",
                   func, err, path, ino);
    } else {
        dfs_printf("%s: reporting error %d for path %s\n", func, err, path);
    }
}

#endif
