#include "includes.h"

/* Take the lock on inode in the specified mode */
void
dfs_inodeLock(struct inode *inode, bool exclusive) {
    if (exclusive) {
        pthread_rwlock_wrlock(&inode->i_rwlock);
    } else {
        pthread_rwlock_rdlock(&inode->i_rwlock);
    }
}

/* Unlock the inode */
void
dfs_inodeUnlock(struct inode *inode) {
    pthread_rwlock_unlock(&inode->i_rwlock);
}

/* Update inode times */
void
dfs_updateInodeTimes(struct inode *inode, bool atime, bool mtime, bool ctime) {
    struct timespec tv;

    clock_gettime(CLOCK_REALTIME, &tv);
    if (atime) {
        inode->i_stat.st_atim = tv;
    }
    if (mtime) {
        inode->i_stat.st_mtim = tv;
    }
    if (ctime) {
        inode->i_stat.st_ctim = tv;
    }
}

/* Initialize root inode of a file system */
static void
dfs_rootInit(struct fs *fs, ino_t root) {
    struct inode *inode = malloc(sizeof(struct inode));

    memset(inode, 0, sizeof(struct inode));
    pthread_rwlock_init(&inode->i_rwlock, NULL);
    inode->i_stat.st_ino = root;
    inode->i_stat.st_mode = S_IFDIR | 0777;
    inode->i_stat.st_nlink = 2;
    inode->i_stat.st_blksize = DFS_BLOCK_SIZE;
    inode->i_parent = root;
    dfs_updateInodeTimes(inode, true, true, true);
    fs->fs_inode[root] = inode;
}

/* Initialize inode table of a file system */
int
dfs_readInodes(struct fs *fs) {
    fs->fs_inode = malloc(DFS_ICACHE_SIZE * sizeof(struct inode *));
    memset(fs->fs_inode, 0, DFS_ICACHE_SIZE * sizeof(struct inode *));
    dfs_rootInit(fs, fs->fs_root);
    return 0;
}

/* Free an inode and associated resources */
static uint64_t
dfs_freeInode(struct inode *inode) {
    uint64_t count = 0;

    if (S_ISREG(inode->i_stat.st_mode)) {
        count = dfs_truncPages(inode, 0);
    } else if (S_ISDIR(inode->i_stat.st_mode)) {
        dfs_dirFree(inode);
    } else if (S_ISLNK(inode->i_stat.st_mode)) {
        free(inode->i_target);
    }
    dfs_xattrFree(inode);
    free(inode);
    return count;
}

/* Destroy inodes belong to a file system */
uint64_t
dfs_destroyInodes(struct fs *fs) {
    struct inode *inode;
    uint64_t count = 0;
    int i;

    for (i = 0; i < DFS_ICACHE_SIZE; i++) {
        inode = fs->fs_inode[i];
        if (inode) {
            count += dfs_freeInode(inode);
        }
    }
    free(fs->fs_inode);
    return count;
}

/* Clone an inode from a parent layer */
struct inode *
dfs_cloneInode(struct fs *fs, struct inode *parent, ino_t ino) {
    struct inode *inode;
    char *target;
    int len;

    inode = malloc(sizeof(struct inode));
    memset(inode, 0, sizeof(struct inode));
    pthread_rwlock_init(&inode->i_rwlock, NULL);
    memcpy(&inode->i_stat, &parent->i_stat, sizeof(struct stat));

    if (S_ISREG(inode->i_stat.st_mode)) {

        /* Share pages initially */
        if (parent->i_page) {
            inode->i_page = parent->i_page;
            inode->i_pcount = parent->i_pcount;
            inode->i_shared = true;
        }
    } else if (S_ISDIR(inode->i_stat.st_mode)) {

        /* Copy directory entries */
        dfs_dirCopy(inode, parent);
    } else if (S_ISLNK(inode->i_stat.st_mode)) {

        /* XXX New inode may share target */
        target = parent->i_target;
        len = strlen(target);
        inode->i_target = malloc(len + 1);
        memcpy(inode->i_target, target, len);
        inode->i_target[len] = 0;
    }
    dfs_xattrCopy(inode, parent);
    fs->fs_inode[ino] = inode;
    return inode;
}

/* Get an inode locked in the requested mode */
struct inode *
dfs_getInode(struct fs *fs, ino_t ino, struct inode *handle,
             bool copy, bool exclusive) {
    ino_t inum = dfs_getInodeHandle(ino);
    struct inode *inode, *parent;
    struct fs *pfs;

    /* Check if the file system has the inode or not */
    inode = fs->fs_inode[inum];
    if (inode) {
        dfs_inodeLock(inode, exclusive);
        return inode;
    }

    /* Check if the file handle points to the inode */
    if (handle && !copy) {
        inode = handle;
        assert(inode->i_stat.st_ino == inum);
        dfs_inodeLock(inode, exclusive);
        return inode;
    }

    /* XXX Reduce the time this lock is held */
    pthread_mutex_lock(&fs->fs_gfs->gfs_ilock);
    inode = fs->fs_inode[inum];
    if (inode == NULL) {
        pfs = fs->fs_parent;
        while (pfs) {
            parent = pfs->fs_inode[inum];
            if (parent != NULL) {

                /* Do not clone if the inode is removed in a parent layer */
                if (!parent->i_removed) {

                    /* Clone the inode only when modified */
                    if (copy) {
                        inode = dfs_cloneInode(fs, parent, inum);
                    } else {
                        inode = parent;
                    }
                }
                break;
            }
            pfs = pfs->fs_parent;
        }
    }
    pthread_mutex_unlock(&fs->fs_gfs->gfs_ilock);

    /* Now lock the inode */
    if (inode) {
        dfs_inodeLock(inode, exclusive);
    } else {
        dfs_printf("Inode is NULL, fs %ld ino %ld\n", fs->fs_root, ino);
    }
    return inode;
}

/* Allocate a new inode */
static ino_t
dfs_inodeAlloc(struct fs *fs) {
    return __sync_add_and_fetch(&fs->fs_gfs->gfs_super->sb_ninode, 1);
}

/* Initialize a newly allocated inode */
struct inode *
dfs_inodeInit(struct fs *fs, mode_t mode, uid_t uid, gid_t gid,
              dev_t rdev, ino_t parent, const char *target) {
    struct inode *inode;
    ino_t ino;
    int len;

    ino = dfs_inodeAlloc(fs);
    assert(ino < DFS_ICACHE_SIZE);
    inode = malloc(sizeof(struct inode));
    memset(inode, 0, sizeof(struct inode));
    pthread_rwlock_init(&inode->i_rwlock, NULL);
    inode->i_stat.st_ino = ino;
    inode->i_stat.st_mode = mode;
    inode->i_stat.st_nlink = (mode & S_IFDIR) ? 2 : 1;
    inode->i_stat.st_uid = uid;
    inode->i_stat.st_gid = gid;
    inode->i_stat.st_rdev = rdev;
    inode->i_stat.st_blksize = DFS_BLOCK_SIZE;
    inode->i_parent = dfs_getInodeHandle(parent);
    dfs_updateInodeTimes(inode, true, true, true);
    if (target != NULL) {
        len = strlen(target);
        inode->i_target = malloc(len + 1);
        memcpy(inode->i_target, target, len);
        inode->i_target[len] = 0;
    }
    dfs_inodeLock(inode, true);
    fs->fs_inode[ino] = inode;
    return inode;
}
