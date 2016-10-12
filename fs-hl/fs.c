#include "includes.h"

/* Get the global file system */
struct gfs *
getfs() {
    struct fuse_context *fc = fuse_get_context();

    return fc->private_data;
}

/* Check if the specified inode is root of a file system and if so, returning
 * corresponding file system.
 */
struct fs *
dfs_getfs(struct gfs *gfs, ino_t root) {
    struct fs *fs;

    fs = gfs->gfs_fs;
    while (fs) {
        if (fs->fs_root == root) {
            return fs;
        }
        fs = fs->fs_gnext;
    }
    return NULL;
}

/* Check if the specified inode is a root of a file system and if so, return
 * the new file system.
 */
struct fs *
dfs_checkfs(struct fs *fs, ino_t ino) {
    struct fs *nfs;

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
