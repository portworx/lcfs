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
    time_t sec = time(NULL);

    if (atime) {
        if (inode->i_stat.st_atime == sec) {
            inode->i_stat.st_atime++;
        } else {
            inode->i_stat.st_atime = sec;
        }
    }
    if (mtime) {
        if (inode->i_stat.st_mtime == sec) {
            inode->i_stat.st_mtime++;
        } else {
            inode->i_stat.st_mtime = sec;
        }
    }
    if (ctime) {
        if (inode->i_stat.st_ctime == sec) {
            inode->i_stat.st_ctime++;
        } else {
            inode->i_stat.st_ctime = sec;
        }
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

    if ((inode->i_stat.st_mode & S_IFMT) == S_IFREG) {

        /* Share pages initially */
        if (parent->i_page) {
            inode->i_page = parent->i_page;
            inode->i_shared = true;
        }
    } else if ((inode->i_stat.st_mode & S_IFMT) == S_IFDIR) {

        /* Copy directory entries */
        dfs_dirCopy(inode, parent);
    } else if ((inode->i_stat.st_mode & S_IFMT) == S_IFLNK) {

        /* XXX New inode may share target */
        target = parent->i_target;
        len = strlen(target);
        inode->i_target = malloc(len + 1);
        memcpy(inode->i_target, target, len);
        inode->i_target[len] = 0;
    }
    fs->fs_inode[ino] = inode;
    return inode;
}

/* Get an inode locked in the requested mode */
struct inode *
dfs_getInode(struct fs *fs, ino_t ino, bool copy, bool exclusive) {
    struct inode *inode, *parent;
    struct fs *pfs;

    inode = fs->fs_inode[ino];
    if (inode) {
        dfs_inodeLock(inode, exclusive);
        return inode;
    }

    /* XXX Reduce the time this lock is held */
    pthread_mutex_lock(&fs->fs_gfs->gfs_ilock);
    inode = fs->fs_inode[ino];
    if (inode == NULL) {
        pfs = fs->fs_parent;
        while (pfs) {
            parent = pfs->fs_inode[ino];
            if (parent != NULL) {

                /* Do not clone if the inode is removed in a parent layer */
                if (!parent->i_removed) {

                    /* Clone the inode only when modified */
                    if (copy) {
                        inode = dfs_cloneInode(fs, parent, ino);
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

/* Given a path, return corresponding inode */
struct inode *
dfs_getPathInode(const char *path, struct gfs *gfs, bool copy, bool exclusive) {
    struct fs *fs;
    ino_t ino = dfs_lookup(path, gfs, &fs, NULL, NULL);

    if (ino == DFS_INVALID_INODE) {
        return NULL;
    }
    return dfs_getInode(fs, ino, copy, exclusive);
}

/* Allocate a new inode */
static ino_t
dfs_inodeAlloc(struct fs *fs) {
    return __sync_add_and_fetch(&fs->fs_gfs->gfs_super->sb_ninode, 1);
}

/* Initialize a newly allocated inode */
ino_t
dfs_inodeInit(struct fs *fs, mode_t mode, uid_t uid, gid_t gid,
              dev_t rdev, const char *target) {
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
    dfs_updateInodeTimes(inode, true, true, true);
    if (target != NULL) {
        len = strlen(target);
        inode->i_target = malloc(len + 1);
        memcpy(inode->i_target, target, len);
        inode->i_target[len] = 0;
    }
    fs->fs_inode[ino] = inode;
    return ino;
}

/* Set up inode handle using inode number and file system id */
uint64_t
dfs_setHandle(struct fs *fs, ino_t ino) {
    return (fs->fs_root << 32) | ino;
}

/* Get the file system id from the file handle */
ino_t
dfs_getFsHandle(uint64_t fh) {
    return fh >> 32;
}

/* Get inode number corresponding to the file handle */
ino_t
dfs_getInodeHandle(uint64_t fh) {
    return fh & 0xFFFFFFFF;
}

