#ifndef __DIFF_H_
#define __DIFF_H_

enum lc_changeType {

    /* Modified */
    LC_MODIFIED = 0,

    /* Newly added */
    LC_ADDED = 1,

    /* Removed */
    LC_REMOVED = 2,
} __attribute__((packed));

/* A file which is added/modified/removed */
struct cfile {

    /* Name of file */
    char *cf_name;

    /* Next file in the list */
    struct cfile *cf_next;

    /* Length of name */
    uint16_t cf_len:14;

    /* Type of change */
    uint16_t cf_type:2;
} __attribute__((packed));

/* A directory which has some files added/modified/removed */
struct cdir {

    /* Inode number of this directory */
    uint64_t cd_ino:62;

    /* Type of change */
    uint64_t cd_type:2;

    /* Parent inode number */
    uint64_t cd_parent:48;

    /* Length of path */
    uint64_t cd_len:16;

    /* Path name to this directory */
    char *cd_path;

    /* Next directory in the list */
    struct cdir *cd_next;

    /* A linked list of files added/modified/removed */
    struct cfile *cd_file;
} __attribute__((packed));

#endif
