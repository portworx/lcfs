#include "includes.h"

/* Allocate a new file system structure */
struct fs *
dfs_newFs(struct gfs *gfs, bool locks) {
    struct fs *fs = malloc(sizeof(struct fs));

    memset(fs, 0, sizeof(*fs));
    fs->fs_gfs = gfs;
    if (locks) {
        fs->fs_rwlock = malloc(sizeof(pthread_rwlock_t));
        pthread_rwlock_init(fs->fs_rwlock, NULL);
    }
    fs->fs_icache = dfs_icache_init();
    return fs;
}

/* Delete a file system */
void
dfs_destroyFs(struct fs *fs) {
    uint64_t count;

    count = dfs_destroyInodes(fs);
    if (count) {
        dfs_blockFree(fs->fs_gfs, count);
    }
    if (fs->fs_rwlock) {
        pthread_rwlock_destroy(fs->fs_rwlock);
        free(fs->fs_rwlock);
    }
    if (fs->fs_ilock && (fs->fs_parent == NULL)) {
        pthread_mutex_destroy(fs->fs_ilock);
        free(fs->fs_ilock);
    }
    free(fs);
}

/* Lock a file system in shared while starting a request.
 * File system is locked in exclusive mode while taking/deleting snapshots.
 */
void
dfs_lock(struct fs *fs, bool exclusive) {
    if (fs->fs_rwlock) {
        if (exclusive) {
            pthread_rwlock_wrlock(fs->fs_rwlock);
        } else {
            pthread_rwlock_rdlock(fs->fs_rwlock);
        }
    }
}

/* Unlock the file system */
void
dfs_unlock(struct fs *fs) {
    if (fs->fs_rwlock) {
        pthread_rwlock_unlock(fs->fs_rwlock);
    }
}

/* Check if the specified inode is a root of a file system */
static int
dfs_checkRoot(struct gfs *gfs, ino_t ino, int index) {
    ino_t root = dfs_getInodeHandle(ino);
    int i;

    for (i = 0; i <= gfs->gfs_scount; i++) {
        if (gfs->gfs_roots[i] == root) {
            return i;
        }
    }
    return index;
}

/* Check if the specified inode is a root of a file system and if so, return
 * the index of the new file system. Otherwise, return the index of current
 * file system.
 */
int
dfs_getIndex(struct fs *nfs, ino_t parent, ino_t ino) {
    struct gfs *gfs = nfs->fs_gfs;
    int gindex = nfs->fs_gindex;

    /* Snapshots are allowed in one directory right now */
    if ((gindex == 0) && gfs->gfs_scount && (parent == gfs->gfs_snap_root)) {
        assert(dfs_globalRoot(ino));
        gindex = dfs_checkRoot(gfs, ino, gindex);
    }
    return gindex;
}

/* Return the file system in which the inode belongs to */
struct fs *
dfs_getfs(ino_t ino, bool exclusive) {
    int gindex = dfs_getFsHandle(ino);
    struct gfs *gfs = getfs();
    struct fs *fs;

    assert(gindex < DFS_FS_MAX);
    fs = gfs->gfs_fs[gindex];
    dfs_lock(fs, exclusive);
    assert(fs->fs_gindex == gindex);
    assert(gfs->gfs_roots[gindex] == fs->fs_root);
    return fs;
}

/* Add a file system to global list of file systems */
void
dfs_addfs(struct fs *fs, struct fs *snap) {
    struct gfs *gfs = fs->fs_gfs;
    int i;

    pthread_mutex_lock(&gfs->gfs_lock);
    for (i = 0; i < DFS_FS_MAX; i++) {
        if (gfs->gfs_fs[i] == NULL) {
            fs->fs_gindex = i;
            gfs->gfs_fs[i] = fs;
            gfs->gfs_roots[i] = fs->fs_root;
            if (i > gfs->gfs_scount) {
                gfs->gfs_scount = i;
            }
            break;
        }
    }
    assert(i < DFS_FS_MAX);

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
    struct fs *pfs, *nfs;

    assert(pfs);
    assert(fs->fs_snap == NULL);
    assert(fs->fs_gindex > 0);
    assert(fs->fs_gindex < DFS_FS_MAX);
    pthread_mutex_lock(&gfs->gfs_lock);
    gfs->gfs_fs[fs->fs_gindex] = NULL;
    gfs->gfs_roots[fs->fs_gindex] = 0;
    if (gfs->gfs_scount == fs->fs_gindex) {
        assert(gfs->gfs_scount > 0);
        gfs->gfs_scount--;
    }

    /* Remove the file system from the snapshot list */
    pfs = fs->fs_parent;
    if (pfs && (pfs->fs_snap == fs)) {
        pfs->fs_snap = fs->fs_next;
    } else {
        if (pfs) {
            nfs = pfs->fs_snap;
        } else {
            nfs = gfs->gfs_fs[0];
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

    memset(gfs, 0, sizeof(struct gfs));
    gfs->gfs_fs = malloc(sizeof(struct fs *) * DFS_FS_MAX);
    memset(gfs->gfs_fs, 0, sizeof(struct fs *) * DFS_FS_MAX);
    gfs->gfs_roots = malloc(sizeof(ino_t) * DFS_FS_MAX);
    memset(gfs->gfs_roots, 0, sizeof(ino_t) * DFS_FS_MAX);
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
    fs = dfs_newFs(gfs, false);
    fs->fs_root = DFS_ROOT_INODE;
    gfs->gfs_fs[0] = fs;
    gfs->gfs_roots[0] = DFS_ROOT_INODE;
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
    struct fs *fs = gfs->gfs_fs[0];

    close(gfs->gfs_fd);
    if (gfs->gfs_super != NULL) {
        free(gfs->gfs_super);
    }
    pthread_mutex_destroy(&gfs->gfs_lock);
    if (fs) {
        dfs_destroyFs(fs);
    }
    free(gfs->gfs_roots);
    free(gfs->gfs_fs);
    free(gfs);
}
