#include "includes.h"

/* Given an inode number, return the hash index */
static inline int
dfs_inodeHash(ino_t ino) {
    return ino % DFS_ICACHE_SIZE;
}

/* Allocate and initialize inode hash table */
struct icache *
dfs_icache_init() {
    struct icache *icache = malloc(sizeof(struct icache) * DFS_ICACHE_SIZE);
    int i;

    for (i = 0; i < DFS_ICACHE_SIZE; i++) {
        pthread_mutex_init(&icache[i].ic_lock, NULL);
        icache[i].ic_head = NULL;
    }
    return icache;
}

/* Allocate a new inode */
static struct inode *
dfs_newInode(struct fs *fs) {
    struct inode *inode;

    inode = malloc(sizeof(struct inode));
    memset(inode, 0, sizeof(struct inode));
    inode->i_block = DFS_INVALID_BLOCK;
    inode->i_bmapDirBlock = DFS_INVALID_BLOCK;
    inode->i_xattrBlock = DFS_INVALID_BLOCK;
    pthread_rwlock_init(&inode->i_rwlock, NULL);
    pthread_rwlock_init(&inode->i_pglock, NULL);

    /* XXX This accounting is not correct after restart */
    __sync_add_and_fetch(&fs->fs_gfs->gfs_super->sb_inodes, 1);
    __sync_add_and_fetch(&fs->fs_icount, 1);
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
    pthread_mutex_lock(&fs->fs_icache[hash].ic_lock);
    inode->i_cnext = fs->fs_icache[hash].ic_head;
    fs->fs_icache[hash].ic_head = inode;
    pthread_mutex_unlock(&fs->fs_icache[hash].ic_lock);
    inode->i_fs = fs;
}

static struct inode *
dfs_lookupInodeCache(struct fs *fs, ino_t ino) {
    int hash = dfs_inodeHash(ino);
    struct inode *inode;

    if (fs->fs_icache[hash].ic_head == NULL) {
        return NULL;
    }
    /* XXX Locking not needed right now, as inodes are not removed */
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

/* Lookup an inode in the hash list */
static struct inode *
dfs_lookupInode(struct fs *fs, ino_t ino) {
    struct gfs *gfs = fs->fs_gfs;

    if (ino == fs->fs_root) {
        return fs->fs_rootInode;
    }
    if (ino == gfs->gfs_snap_root) {
        return gfs->gfs_snap_rootInode;
    }
    return dfs_lookupInodeCache(fs, ino);
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
void
dfs_rootInit(struct fs *fs, ino_t root) {
    struct inode *inode = dfs_newInode(fs);

    inode->i_stat.st_ino = root;
    inode->i_stat.st_mode = S_IFDIR | 0755;
    inode->i_stat.st_nlink = 2;
    inode->i_stat.st_blksize = DFS_BLOCK_SIZE;
    inode->i_parent = root;
    dfs_updateInodeTimes(inode, true, true, true);
    inode->i_fs = fs;
    dfs_addInode(fs, inode);
    fs->fs_rootInode = inode;
    dfs_markInodeDirty(inode, true, true, false, false);
}

/* Set up snapshot root inode */
void
dfs_setSnapshotRoot(struct gfs *gfs, ino_t ino) {
    if (gfs->gfs_snap_root) {
        if (gfs->gfs_scount) {
            printf("Warning: Snapshot root changed when snapshots are present\n");
        }
        printf("Switching snapshot root from %ld to %ld\n", gfs->gfs_snap_root, ino);
        gfs->gfs_snap_root = 0;
    }
    gfs->gfs_snap_rootInode = dfs_getInode(dfs_getGlobalFs(gfs), ino,
                                           NULL, false, false);
    assert(S_ISDIR(gfs->gfs_snap_rootInode->i_stat.st_mode));
    dfs_inodeUnlock(gfs->gfs_snap_rootInode);
    gfs->gfs_snap_root = ino;
    printf("snapshot root inode %ld\n", ino);
}

/* Initialize inode table of a file system */
int
dfs_readInodes(struct gfs *gfs, struct fs *fs) {
    uint64_t iblock, block = fs->fs_super->sb_inodeBlock;
    struct inode *inode;
    bool flush = false;
    char *target;
    void *ibuf;
    int i;

    dfs_printf("Reading inodes for fs %d %ld\n", fs->fs_gindex, fs->fs_root);
    while (block != DFS_INVALID_BLOCK) {
        //dfs_printf("Reading inode table from block %ld\n", block);
        fs->fs_inodeBlocks = dfs_readBlock(gfs->gfs_fd, block);
        for (i = 0; i < DFS_IBLOCK_MAX; i++) {
            iblock = fs->fs_inodeBlocks->ib_blks[i];
            if (iblock == 0) {
                break;
            }
            if (iblock == DFS_INVALID_BLOCK) {
                //dfs_printf("Skipping removed inode, iblock is DFS_INVALID_BLOCK\n");
                continue;
            }
            //dfs_printf("Reading inode from block %ld\n", iblock);
            ibuf = dfs_readBlock(gfs->gfs_fd, iblock);
            inode = ibuf;
            if (inode->i_stat.st_ino == 0) {
                //dfs_printf("Skipping removed inode\n");
                fs->fs_inodeBlocks->ib_blks[i] = DFS_INVALID_BLOCK;
                flush = true;
                free(ibuf);
                continue;
            }
            inode = malloc(sizeof(struct inode));
            __sync_add_and_fetch(&fs->fs_icount, 1);

            /* XXX zero out just necessary fields */
            memset(inode, 0, sizeof(struct inode));
            memcpy(inode, ibuf, sizeof(struct dinode));
            inode->i_block = iblock;
            pthread_rwlock_init(&inode->i_rwlock, NULL);
            pthread_rwlock_init(&inode->i_pglock, NULL);
            dfs_addInode(fs, inode);
            dfs_xattrRead(gfs, fs, inode);
            if (S_ISREG(inode->i_stat.st_mode)) {
                dfs_bmapRead(gfs, fs, inode);
            } else if (S_ISDIR(inode->i_stat.st_mode)) {
                dfs_dirRead(gfs, fs, inode);
            } else if (S_ISLNK(inode->i_stat.st_mode)) {
                inode->i_target = malloc(inode->i_stat.st_size + 1);
                target = ibuf;
                target += sizeof(struct dinode);
                memcpy(inode->i_target, target, inode->i_stat.st_size);
                inode->i_target[inode->i_stat.st_size] = 0;
            }
            if (inode->i_stat.st_ino == fs->fs_root) {
                assert(S_ISDIR(inode->i_stat.st_mode));
                fs->fs_rootInode = inode;
            }
            free(ibuf);
        }
        if (flush) {
            dfs_writeBlock(gfs->gfs_fd, fs->fs_inodeBlocks, block);
            flush = false;
        }
        block = fs->fs_inodeBlocks->ib_next;
        free(fs->fs_inodeBlocks);
    }
    assert(fs->fs_rootInode != NULL);
    fs->fs_inodeBlocks = NULL;
    return 0;
}

/* Free an inode and associated resources */
static uint64_t
dfs_freeInode(struct inode *inode, bool remove) {
    uint64_t count = 0;

    if (S_ISREG(inode->i_stat.st_mode)) {
        count = dfs_truncPages(inode, 0, remove);
    } else if (S_ISDIR(inode->i_stat.st_mode)) {
        dfs_dirFree(inode);
    } else if (S_ISLNK(inode->i_stat.st_mode)) {
        if (!inode->i_shared) {
            free(inode->i_target);
        }
        inode->i_target = NULL;
    }
    assert(inode->i_page == NULL);
    assert(inode->i_bmap == NULL);
    dfs_xattrFree(inode);
    pthread_rwlock_destroy(&inode->i_pglock);
    pthread_rwlock_destroy(&inode->i_rwlock);
    free(inode);
    return count;
}

/* Flush a dirty inode to disk */
static void
dfs_flushInode(struct gfs *gfs, struct fs *fs, struct inode *inode) {
    char *buf;

    assert(inode->i_fs == fs);
    if (inode->i_xattrdirty) {
        dfs_xattrFlush(gfs, fs, inode);
    }

    if (inode->i_bmapdirty) {
        dfs_bmapFlush(gfs, fs, inode);
    }

    if (inode->i_dirdirty) {
        dfs_dirFlush(gfs, fs, inode);
    }

    /* Write out a dirty inode */
    if (inode->i_dirty) {
        if (!inode->i_removed) {
            if (inode->i_block == DFS_INVALID_BLOCK) {
                if ((fs->fs_inodeBlocks == NULL) ||
                    (fs->fs_inodeIndex >= DFS_IBLOCK_MAX)) {
                    dfs_newInodeBlock(gfs, fs);
                }
                inode->i_block = dfs_blockAlloc(fs, 1, true);
                fs->fs_inodeBlocks->ib_blks[fs->fs_inodeIndex++] = inode->i_block;
            }
            //dfs_printf("Writing inode %ld to block %ld\n", inode->i_stat.st_ino, inode->i_block);
            posix_memalign((void **)&buf, DFS_BLOCK_SIZE, DFS_BLOCK_SIZE);
            memcpy(buf, &inode->i_dinode, sizeof(struct dinode));
            if (S_ISLNK(inode->i_stat.st_mode)) {
                memcpy(&buf[sizeof(struct dinode)], inode->i_target,
                       inode->i_stat.st_size);
            }
            dfs_writeBlock(gfs->gfs_fd, buf, inode->i_block);
            free(buf);
        } else if (inode->i_block != DFS_INVALID_BLOCK) {
            inode->i_stat.st_ino = 0;
            posix_memalign((void **)&buf, DFS_BLOCK_SIZE, DFS_BLOCK_SIZE);
            memcpy(buf, &inode->i_dinode, sizeof(struct dinode));
            dfs_writeBlock(gfs->gfs_fd, buf, inode->i_block);
            free(buf);
        }
        inode->i_dirty = false;
    }
}

/* Sync all dirty inodes */
void
dfs_syncInodes(struct gfs *gfs, struct fs *fs) {
    struct inode *inode;
    int i;

    dfs_printf("Syncing inodes for fs %d %ld\n", fs->fs_gindex, fs->fs_root);
    for (i = 0; i < DFS_ICACHE_SIZE; i++) {
        inode = fs->fs_icache[i].ic_head;
        if (inode == NULL) {
            continue;
        }
        while (inode) {
            if (dfs_inodeDirty(inode)) {
                dfs_flushInode(gfs, fs, inode);
            }
            inode = inode->i_cnext;
        }
    }
    if (fs->fs_inodeBlocks != NULL) {
        assert(fs->fs_super->sb_inodeBlock != DFS_INVALID_BLOCK);
        //dfs_printf("Writing Inode block at %ld\n", fs->fs_super->sb_inodeBlock);
        dfs_writeBlock(gfs->gfs_fd, fs->fs_inodeBlocks,
                       fs->fs_super->sb_inodeBlock);
        free(fs->fs_inodeBlocks);
        fs->fs_inodeBlocks = NULL;
        fs->fs_inodeIndex = 0;
    }
}

/* Invalidate kernel page cache for a file system */
void
dfs_invalidate_pcache(struct gfs *gfs, struct fs *fs) {
    int gindex = fs->fs_gindex;
    struct inode *inode;
    int i;

    for (i = 0; i < DFS_ICACHE_SIZE; i++) {
        inode = fs->fs_icache[i].ic_head;
        if (inode == NULL) {
            continue;
        }
        /* XXX Locking not needed right now, as inodes are not removed */
        //pthread_mutex_lock(&fs->fs_icache[i].ic_lock);
        while (inode) {
            assert(inode->i_fs == fs);
            if (inode->i_pcache && (inode->i_stat.st_size > 0)) {
                assert(S_ISREG(inode->i_stat.st_mode));
                fuse_lowlevel_notify_inval_inode(gfs->gfs_ch,
                                                 dfs_setHandle(gindex,
                                                         inode->i_stat.st_ino),
                                                         0, -1);
            }
            inode = inode->i_cnext;
        }
        //pthread_mutex_unlock(&fs->fs_icache[i].ic_lock);
    }
}

/* Destroy inodes belong to a file system */
uint64_t
dfs_destroyInodes(struct fs *fs, bool remove) {
    uint64_t count = 0, icount = 0;
    struct inode *inode;
    int i;

    /* Take the inode off the hash list */

    for (i = 0; i < DFS_ICACHE_SIZE; i++) {
        /* XXX Lock is not needed as the file system is locked for exclusive
         * access
         * */
        //pthread_mutex_lock(&fs->fs_icache[i].ic_lock);
        while ((inode = fs->fs_icache[i].ic_head)) {
            fs->fs_icache[i].ic_head = inode->i_cnext;
            count += dfs_freeInode(inode, remove);
            icount++;
        }
        assert(fs->fs_icache[i].ic_head == NULL);
        //pthread_mutex_unlock(&fs->fs_icache[i].ic_lock);
        pthread_mutex_destroy(&fs->fs_icache[i].ic_lock);
    }
    /* XXX reuse this cache for another file system */
    free(fs->fs_icache);
    if (remove && icount) {
        __sync_sub_and_fetch(&fs->fs_gfs->gfs_super->sb_inodes, icount);
    }
    if (icount) {
        __sync_sub_and_fetch(&fs->fs_icount, icount);
    }
    return remove ? count : 0;
}

/* Clone an inode from a parent layer */
struct inode *
dfs_cloneInode(struct fs *fs, struct inode *parent, ino_t ino) {
    struct inode *inode;

    inode = dfs_newInode(fs);
    memcpy(&inode->i_stat, &parent->i_stat, sizeof(struct stat));

    if (S_ISREG(inode->i_stat.st_mode)) {
        assert(parent->i_page == NULL);

        /* Share pages initially */
        if (parent->i_stat.st_blocks) {
            if (parent->i_extentLength) {
                inode->i_extentBlock = parent->i_extentBlock;
                inode->i_extentLength = parent->i_extentLength;
            } else {
                inode->i_bmap = parent->i_bmap;
                inode->i_bcount = parent->i_bcount;
                inode->i_bmapdirty = true;
            }
            inode->i_shared = true;
        } else {
            inode->i_pcache = true;
        }
    } else if (S_ISDIR(inode->i_stat.st_mode)) {
        if (parent->i_dirent) {
            inode->i_dirent = parent->i_dirent;
            inode->i_shared = true;
            inode->i_dirdirty = true;
        }
    } else if (S_ISLNK(inode->i_stat.st_mode)) {
        inode->i_target = parent->i_target;
        inode->i_shared = true;
    }
    inode->i_parent = (parent->i_parent == parent->i_fs->fs_root) ?
                      fs->fs_root : parent->i_parent;
    dfs_xattrCopy(inode, parent);
    dfs_addInode(fs, inode);
    inode->i_dirty = true;
    return inode;
}

/* Lookup the requested inode in the chain */
static struct inode *
dfs_getInodeParent(struct fs *fs, ino_t inum, bool copy) {
    struct inode *inode, *parent;
    struct fs *pfs;

    /* XXX Reduce the time this lock is held */
    pthread_mutex_lock(fs->fs_ilock);
    inode = dfs_lookupInodeCache(fs, inum);
    if (inode == NULL) {
        pfs = fs->fs_parent;
        while (pfs) {
            parent = dfs_lookupInodeCache(pfs, inum);
            if (parent != NULL) {

                /* Do not clone if the inode is removed in a parent layer */
                if (!parent->i_removed) {

                    /* Clone the inode only when modified */
                    if (copy) {
                        assert(fs->fs_snap == NULL);
                        inode = dfs_cloneInode(fs, parent, inum);
                    } else {
                        /* XXX Remember this for future lookup */
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
    inode->i_stat.st_nlink = S_ISDIR(mode) ? 2 : 1;
    inode->i_stat.st_uid = uid;
    inode->i_stat.st_gid = gid;
    inode->i_stat.st_rdev = rdev;
    inode->i_stat.st_blksize = DFS_BLOCK_SIZE;
    inode->i_parent = dfs_getInodeHandle(parent);
    inode->i_pcache = S_ISREG(mode);
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
