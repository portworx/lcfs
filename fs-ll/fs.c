#include "includes.h"

/* Allocate a new file system structure */
struct fs *
dfs_newFs(struct gfs *gfs, ino_t root, bool locks) {
    struct fs *fs = malloc(sizeof(struct fs));

    memset(fs, 0, sizeof(*fs));
    fs->fs_root = root;
    if (locks) {
        fs->fs_ilock = malloc(sizeof(pthread_mutex_t));
        pthread_mutex_init(fs->fs_ilock, NULL);
        fs->fs_rwlock = malloc(sizeof(pthread_rwlock_t));
        pthread_rwlock_init(fs->fs_rwlock, NULL);
    }
    fs->fs_gfs = gfs;
    return fs;
}

/* Delete a file system */
void
dfs_removeFs(struct fs *fs) {
    struct gfs *gfs = getfs();
    uint64_t count;

    count = dfs_destroyInodes(fs);
    if (count) {
        dfs_blockFree(gfs, count);
    }
    if ((fs->fs_parent == NULL) && (fs->fs_root != DFS_ROOT_INODE)) {
        pthread_rwlock_destroy(fs->fs_rwlock);
        pthread_mutex_destroy(fs->fs_ilock);
        free(fs->fs_ilock);
    }
    free(fs);
}

/* Lock a file system in shared while starting a request.
 * File system is locked in exclusive mode while taking/deleting snapshots.
 */
static inline void
dfs_lock(struct fs *fs, bool exclusive) {
    if (fs->fs_root == DFS_ROOT_INODE) {
        return;
    }
    if (exclusive) {
        pthread_rwlock_wrlock(fs->fs_rwlock);
    } else {
        pthread_rwlock_rdlock(fs->fs_rwlock);
    }
}

/* Unlock the file system */
void
dfs_unlock(struct fs *fs) {
    if (fs->fs_root != DFS_ROOT_INODE) {
        pthread_rwlock_unlock(fs->fs_rwlock);
    }
}

/* Check if the specified inode is root of a file system and if so, returning
 * corresponding file system.
 */
struct fs *
dfs_getfs(struct gfs *gfs, ino_t ino, bool exclusive) {
    ino_t root = dfs_getFsHandle(ino);
    struct fs *fs, *rfs = NULL;

    /* If inode knows the file system it belongs to, look for that */
    if (root == DFS_ROOT_INODE) {
        root = dfs_getInodeHandle(ino);
        if (root < DFS_ROOT_INODE) {
            root = DFS_ROOT_INODE;
        }
    }
    pthread_mutex_lock(&gfs->gfs_lock);
    fs = gfs->gfs_fs;
    while (fs) {
        if (fs->fs_root == root) {
            rfs = fs;
            break;
        }

        /* Remember global file system */
        if (fs->fs_root == DFS_ROOT_INODE) {
            rfs = fs;
        }
        fs = fs->fs_gnext;
    }

    /* XXX Protect the fs while gfs_lock is unlocked */
    pthread_mutex_unlock(&gfs->gfs_lock);
    dfs_lock(rfs, exclusive);
    return rfs;
}

/* Check if the specified inode is a root of a file system and if so, return
 * the new file system. Otherwise, use the current file system.
 */
ino_t
dfs_getRoot(struct fs *nfs, ino_t parent, ino_t ino) {
    ino_t root = dfs_getFsHandle(ino), nroot = nfs->fs_root;
    struct gfs *gfs = nfs->fs_gfs;
    struct fs *fs;

    /* Snapshots are allowed in one directory right now */
    if ((ino > DFS_ROOT_INODE) && (parent == gfs->gfs_snap_root)) {
        pthread_mutex_lock(&gfs->gfs_lock);
        fs = gfs->gfs_fs;
        while (fs) {
            if (fs->fs_root == root) {
                nroot = fs->fs_root;
                break;
            }
            fs = fs->fs_gnext;
        }
        pthread_mutex_unlock(&gfs->gfs_lock);
    }
    return nroot;
}

/* Add a file system to global list of file systems */
void
dfs_addfs(struct fs *fs, struct fs *snap) {
    struct gfs *gfs = fs->fs_gfs;
    struct fs *pfs = gfs->gfs_fs;

    assert(pfs);
    pthread_mutex_lock(&gfs->gfs_lock);
    while (pfs) {
        if (pfs->fs_gnext == NULL) {
            pfs->fs_gnext = fs;
            break;
        }
        pfs = pfs->fs_gnext;
    }

    /* Add this file system to the snapshot list or root file systems list */
    if (snap) {
        fs->fs_next = snap->fs_next;
        snap->fs_next = fs;
    }

    pthread_mutex_unlock(&gfs->gfs_lock);
}

/* Remove a file system from the global list */
void
dfs_removefs(struct fs *fs) {
    struct gfs *gfs = fs->fs_gfs;
    struct fs *pfs = gfs->gfs_fs, *nfs;

    assert(pfs);
    assert(fs->fs_snap == NULL);
    pthread_mutex_lock(&gfs->gfs_lock);
    if (pfs == fs) {
        assert(false);
        gfs->gfs_fs = fs->fs_gnext;
    } else {
        while (pfs) {
            if (pfs->fs_gnext == fs) {
                pfs->fs_gnext = fs->fs_gnext;
                break;
            }
            pfs = pfs->fs_gnext;
        }
    }

    /* Remove the file system from the snapshot list */
    pfs = fs->fs_parent;
    if (pfs && (pfs->fs_snap == fs)) {
        pfs->fs_snap = fs->fs_next;
    } else {
        if (pfs) {
            nfs = pfs->fs_snap;
        } else {
            nfs = gfs->gfs_fs;
        }
        while (nfs) {
            if (nfs->fs_next == fs) {
                nfs->fs_next = fs->fs_next;
                break;
            }
            nfs = nfs->fs_next;
        }
    }
    pthread_mutex_unlock(&gfs->gfs_lock);
}

/* Format a file system by initializing its super block */
void
dfs_format(struct gfs *gfs, size_t size) {
    struct super *sb = gfs->gfs_super;

    memset(sb, 0, sizeof(struct super));
    sb->sb_version = DFS_VERSION;
    sb->sb_magic = DFS_SUPER_MAGIC;
    sb->sb_nblock = DFS_START_BLOCK;
    sb->sb_ninode = DFS_START_INODE;
    sb->sb_tblocks = size / DFS_BLOCK_SIZE;
}

/* Allocate global file system */
static struct gfs *
dfs_gfsAlloc(int fd) {
    struct gfs *gfs = malloc(sizeof(struct gfs));
    pthread_mutex_init(&gfs->gfs_lock, NULL);
    gfs->gfs_fd = fd;
    return gfs;
}

/* Mount the device */
int
dfs_mount(char *device, struct gfs **gfsp) {
    struct gfs *gfs;
    struct fs *fs;
    size_t size;
    int fd, err;

    /* Open the device for mounting */
    fd = open(device, O_RDWR | O_SYNC | O_DIRECT | O_EXCL, 0);
    if (fd == -1) {
        perror("open");
        return errno;
    }

    /* Find the size of the device and calculate total blocks */
    size = lseek(fd, 0, SEEK_END);
    if (size == -1) {
        perror("lseek");
        return errno;
    }
    gfs = dfs_gfsAlloc(fd);

    /* Try to find a valid superblock, if not found, format the device */
    err = dfs_superRead(gfs);
    if (err != 0) {
        printf("Superblock read failed, err %d\n", err);
        return EIO;
    }
    if (gfs->gfs_super->sb_version != DFS_VERSION) {
        dfs_format(gfs, size);
    } else {
        gfs->gfs_super->sb_mounts++;
    }

    /* Initialize a file system structure in memory */
    fs = dfs_newFs(gfs, DFS_ROOT_INODE, false);
    gfs->gfs_fs = fs;
    err = dfs_readInodes(fs);
    if (err != 0) {
        printf("Reading inodes failed, err %d\n", err);
        return EIO;
    }

    /* Write out the file system super block */
    err = dfs_superWrite(gfs);
    if (err != 0) {
        printf("Superblock write failed, err %d\n", err);
    } else {
        *gfsp = gfs;
    }
    return err;
}

/* Free the global file system as part of unmount */
void
dfs_unmount(struct gfs *gfs) {
    close(gfs->gfs_fd);
    if (gfs->gfs_super != NULL) {
        free(gfs->gfs_super);
    }
    pthread_mutex_destroy(&gfs->gfs_lock);
    free(gfs);
}
