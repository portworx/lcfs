#ifndef _INLINES_H
#define _INLINES_H

/* Display debug information on every file system request */
static inline void
dfs_displayEntry(const char *func, const char *path, ino_t ino) {
    /*
    if (strstr(func, "xattr") == NULL) {
        return;
    }
    */
    if ((path == NULL) || ((strstr(path, "childdir") == NULL) && (strstr(path, "testdir") == NULL))) {
        return;
    }
    if (ino) {
        printf("%s: path %s, ino %ld\n", func, path, ino);
    } else {
        printf("%s: path %s\n", func, path);
    }
}

#endif
