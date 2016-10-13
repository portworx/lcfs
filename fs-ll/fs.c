#include "includes.h"

/* Check if the specified inode is root of a file system and if so, returning
 * corresponding file system.
 */
struct fs *
dfs_getfs(struct gfs *gfs, ino_t ino) {
    struct fs *fs = gfs->gfs_fs, *rfs = NULL;
    ino_t root = dfs_getFsHandle(ino);

    /* If inode knows the file system it belongs to, look for that */
    if (root == DFS_ROOT_INODE) {
        root = dfs_getInodeHandle(ino);
        if (root < DFS_ROOT_INODE) {
            root = DFS_ROOT_INODE;
        }
    }
    while (fs) {
        if (fs->fs_root == root) {
            return fs;
        }
        if (fs->fs_root == DFS_ROOT_INODE) {
            rfs = fs;
        }
        fs = fs->fs_gnext;
    }
    return rfs;
}

/* Check if the specified inode is a root of a file system and if so, return
 * the new file system.
 */
struct fs *
dfs_checkfs(struct fs *fs, ino_t ino) {
    struct fs *nfs;

    if (ino <= DFS_ROOT_INODE) {
        return fs;
    }

    /* A given path cannot cross more than one file system, so if called in the
     * context of a snapshot, nothing more to check.
     */
    if (fs->fs_root != DFS_ROOT_INODE) {
        return fs;
    }
    nfs = dfs_getfs(fs->fs_gfs, ino);
    return nfs ? nfs : fs;
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

/* Lock a file system in shared while starting a request.
 * File system is locked in exclusive mode while taking/deleting snapshots.
 */
void
dfs_lock(struct gfs *gfs, bool exclusive) {
    if (exclusive) {
        pthread_rwlock_wrlock(&gfs->gfs_rwlock);
    } else {
        pthread_rwlock_rdlock(&gfs->gfs_rwlock);
    }
}

/* Unlock the file system */
void
dfs_unlock(struct gfs *gfs) {
    pthread_rwlock_unlock(&gfs->gfs_rwlock);
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
    gfs = malloc(sizeof(struct gfs));
    pthread_mutex_init(&gfs->gfs_ilock, NULL);
    pthread_rwlock_init(&gfs->gfs_rwlock, NULL);
    gfs->gfs_fd = fd;

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
    fs = malloc(sizeof(struct fs));
    memset(fs, 0, sizeof(struct fs));
    fs->fs_root = DFS_ROOT_INODE;
    fs->fs_gfs = gfs;
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

/* Delete a file system */
uint64_t
dfs_removeFs(struct fs *fs) {
    uint64_t count;

    count = dfs_destroyInodes(fs);
    free(fs);
    return count;
}

