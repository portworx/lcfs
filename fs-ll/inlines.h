#ifndef _INLINES_H
#define _INLINES_H

static inline ino_t dfs_getFsHandle(uint64_t fh);
static inline ino_t dfs_getInodeHandle(uint64_t fh);

#if 0
#define dfs_printf  printf
#else
#define dfs_printf(a...)
#endif

#if 0
/* Display debug information on every file system request */
static inline void
dfs_displayEntry(const char *func, ino_t dir, ino_t ino, const char *name) {
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
    if ((name == NULL) || strstr(name, "dfs") == NULL) {
        return;
    }
    */
    printf("%s: ino1 %ld (%ld root %ld) ino2 %ld (%ld root %ld) %s\n", func, dir, dfs_getInodeHandle(dir), dfs_getFsHandle(dir), ino, dfs_getInodeHandle(ino), dfs_getFsHandle(ino), name ? name : "");
}
#else
#define dfs_displayEntry(a...)
#endif

#if 1
/* Report errors reported from file system operations */
static inline void
dfs_reportError(const char *func, int line, ino_t ino, int err) {
    printf("%s:%d: reporting error %d for inode %ld (%ld at root %ld)\n", func, line, err, ino, dfs_getInodeHandle(ino), dfs_getFsHandle(ino));
}
#else
#define dfs_reportError(a...)
#endif

#endif
