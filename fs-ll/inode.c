#include "includes.h"

/* Given an inode number, return the hash index */
static inline int
dfs_inodeHash(ino_t ino) {
    return ino / DFS_ICACHE_SIZE;
}

/* Allocate and initialize inode hash table */
struct icache *
dfs_icache_init() {
    struct icache * icache = malloc(sizeof(struct icache) * DFS_ICACHE_SIZE);
    /*
    int i;

    for (i = 0; i < DFS_ICACHE_SIZE; i++) {
        pthread_mutex_init(&icache[i].ic_lock, NULL);
        icache[i].ic_head = NULL;
    }
    */
    memset(icache, 0, sizeof(struct icache) * DFS_ICACHE_SIZE);
    return icache;
}

/* Allocate a new inode */
static struct inode *
dfs_newInode(struct fs *fs) {
    struct inode *inode = malloc(sizeof(struct inode));

    memset(inode, 0, sizeof(struct inode));
    pthread_rwlock_init(&inode->i_rwlock, NULL);
    __sync_add_and_fetch(&fs->fs_gfs->gfs_ninode, 1);
    return inode;
}

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

/* Add an inode to the hash and file system list */
static void
dfs_addInode(struct fs *fs, struct inode *inode) {
    int hash = dfs_inodeHash(inode->i_stat.st_ino);

    /* Add the inode to the hash list */
    /* XXX Have a separate lock for each hash list */
    //pthread_mutex_lock(&fs->fs_icache[hash].ic_lock);
    inode->i_cnext = fs->fs_icache[hash].ic_head;
    fs->fs_icache[hash].ic_head = inode;
    //pthread_mutex_unlock(&fs->fs_icache[hash].ic_lock);
    inode->i_fs = fs;
}

/* Lookup an inode in the hash list */
static struct inode *
dfs_lookupInode(struct fs *fs, ino_t ino) {
    int hash = dfs_inodeHash(ino);
    struct gfs *gfs = fs->fs_gfs;
    struct inode *inode;

    if (ino == fs->fs_root) {
        return fs->fs_rootInode;
    }
    if (fs->fs_icache[hash].ic_head == NULL) {
        return NULL;
    }
    if (ino == gfs->gfs_snap_root) {
        return gfs->gfs_snap_rootInode;
    }
    //pthread_mutex_lock(&fs->fs_icache[hash].ic_lock);
    inode = fs->fs_icache[hash].ic_head;
    while (inode) {
        if (inode->i_stat.st_ino == ino) {
            break;
        }
        inode = inode->i_cnext;
    }
    //pthread_mutex_unlock(&fs->fs_icache[hash].ic_lock);
    return inode;
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
    struct inode *inode = dfs_newInode(fs);

    inode->i_stat.st_ino = root;
    inode->i_stat.st_mode = S_IFDIR | 0777;
    inode->i_stat.st_nlink = 2;
    inode->i_stat.st_blksize = DFS_BLOCK_SIZE;
    inode->i_parent = root;
    dfs_updateInodeTimes(inode, true, true, true);
    inode->i_fs = fs;
    fs->fs_rootInode = inode;
}

/* Initialize inode table of a file system */
int
dfs_readInodes(struct fs *fs) {
    dfs_rootInit(fs, fs->fs_root);
    return 0;
}

/* Free an inode and associated resources */
static uint64_t
dfs_freeInode(struct inode *inode) {
    int hash = dfs_inodeHash(inode->i_stat.st_ino);
    struct fs *fs = inode->i_fs;
    struct inode *pinode;
    uint64_t count = 0;

    /* Take the inode off the hash list */
    //pthread_mutex_lock(&fs->fs_icache[hash].ic_lock);
    pinode = fs->fs_icache[hash].ic_head;
    if (pinode == inode) {
        fs->fs_icache[hash].ic_head = inode->i_cnext;
    } else while (pinode) {
        if (pinode->i_cnext == inode) {
            pinode->i_cnext = inode->i_cnext;
            break;
        }
        pinode = pinode->i_cnext;
    }
    //pthread_mutex_unlock(&fs->fs_icache[hash].ic_lock);

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
    uint64_t count = 0, icount = 0;
    struct inode *inode, *tmp;
    int i;

    for (i = 0; i < DFS_ICACHE_SIZE; i++) {
        inode = fs->fs_icache[i].ic_head;
        while (inode) {
            tmp = inode;
            inode = inode->i_cnext;
            count += dfs_freeInode(tmp);
            icount++;
        }
        //pthread_mutex_destroy(&fs->fs_icache[i].ic_lock);
    }
    free(fs->fs_icache);
    if (icount) {
        __sync_sub_and_fetch(&fs->fs_gfs->gfs_ninode, icount);
    }
    return count;
}

/* Clone an inode from a parent layer */
struct inode *
dfs_cloneInode(struct fs *fs, struct inode *parent, ino_t ino) {
    struct inode *inode;
    char *target;
    int len;

    inode = dfs_newInode(fs);
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
    inode->i_parent = (parent->i_parent == parent->i_fs->fs_root) ?
                      fs->fs_root : parent->i_parent;
    dfs_xattrCopy(inode, parent);
    dfs_addInode(fs, inode);
    return inode;
}

static struct inode *
dfs_getInodeParent(struct fs *fs, ino_t inum, bool copy) {
    struct inode *inode, *parent;
    struct fs *pfs;

    /* XXX Reduce the time this lock is held */
    pthread_mutex_lock(fs->fs_ilock);
    inode = dfs_lookupInode(fs, inum);
    if (inode == NULL) {
        pfs = fs->fs_parent;
        while (pfs) {
            parent = dfs_lookupInode(pfs, inum);
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
    pthread_mutex_unlock(fs->fs_ilock);
    return inode;
}

/* Get an inode locked in the requested mode */
struct inode *
dfs_getInode(struct fs *fs, ino_t ino, struct inode *handle,
             bool copy, bool exclusive) {
    ino_t inum = dfs_getInodeHandle(ino);
    struct inode *inode;

    /* Check if the file handle points to the inode */
    if (handle) {
        inode = handle;
        if (!copy || (inode->i_fs == fs)) {
            assert(inode->i_stat.st_ino == inum);
            dfs_inodeLock(inode, exclusive);
            return inode;
        }
    }

    /* Check if the file system has the inode or not */
    inode = dfs_lookupInode(fs, inum);
    if (inode) {
        dfs_inodeLock(inode, exclusive);
        return inode;
    }

    /* Lookup inode in the parent chain */
    if (fs->fs_parent) {
        inode = dfs_getInodeParent(fs, inum, copy);
    }

    /* Now lock the inode */
    if (inode) {
        dfs_inodeLock(inode, exclusive);
    } else {
        dfs_printf("Inode is NULL, fs gindex %d root %ld ino %ld\n",
                   fs->fs_gindex, fs->fs_root, ino);
    }
    return inode;
}

/* Allocate a new inode */
ino_t
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
    inode = dfs_newInode(fs);
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
        inode->i_stat.st_size = len;
    }
    dfs_inodeLock(inode, true);
    dfs_addInode(fs, inode);
    return inode;
}
