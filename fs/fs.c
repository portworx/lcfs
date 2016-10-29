#include "includes.h"

/* Allocate a new file system structure */
struct fs *
dfs_newFs(struct gfs *gfs, bool rw, bool snapshot) {
    struct fs *fs = malloc(sizeof(struct fs));
    time_t t;

    t = time(NULL);
    memset(fs, 0, sizeof(*fs));
    fs->fs_gfs = gfs;
    fs->fs_readOnly = !rw;
    fs->fs_ctime = t;
    fs->fs_atime = t;
    if (snapshot) {
        fs->fs_rwlock = malloc(sizeof(pthread_rwlock_t));
        pthread_rwlock_init(fs->fs_rwlock, NULL);
    }
    fs->fs_icache = dfs_icache_init();
    fs->fs_stats = dfs_statsNew();
    return fs;
}

/* Delete a file system */
void
dfs_destroyFs(struct fs *fs) {
    uint64_t count;

    dfs_displayStats(fs);
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
    dfs_statsDeinit(fs);
    free(fs->fs_super);
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

/* Check if the specified inode is a root of a file system and if so, return
 * the index of the new file system. Otherwise, return the index of current
 * file system.
 */
int
dfs_getIndex(struct fs *nfs, ino_t parent, ino_t ino) {
    struct gfs *gfs = nfs->fs_gfs;
    int i, gindex = nfs->fs_gindex;
    ino_t root;

    /* Snapshots are allowed in one directory right now */
    if ((gindex == 0) && gfs->gfs_scount && (parent == gfs->gfs_snap_root)) {
        root = dfs_getInodeHandle(ino);
        assert(dfs_globalRoot(ino));
        for (i = 1; i <= gfs->gfs_scount; i++) {
            if (gfs->gfs_roots[i] == root) {
                return i;
            }
        }
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
    for (i = 1; i < DFS_FS_MAX; i++) {
        if (gfs->gfs_fs[i] == NULL) {
            fs->fs_gindex = i;
            fs->fs_super->sb_index = i;
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
        fs->fs_super->sb_nextSnap = snap->fs_super->sb_nextSnap;
        snap->fs_super->sb_nextSnap = fs->fs_sblock;
    }
    pthread_mutex_unlock(&gfs->gfs_lock);
}

/* Remove a file system from the global list */
void
dfs_removefs(struct gfs *gfs, struct fs *fs) {
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
    pthread_mutex_unlock(&gfs->gfs_lock);
}

/* Remove the file system from the snapshot list */
void
dfs_removeSnap(struct gfs *gfs, struct fs *fs) {
    struct fs *pfs, *nfs;

    assert(fs->fs_snap == NULL);
    assert(fs->fs_gindex > 0);
    assert(fs->fs_gindex < DFS_FS_MAX);
    pthread_mutex_lock(&gfs->gfs_lock);
    pfs = fs->fs_parent;
    if (pfs && (pfs->fs_snap == fs)) {
        pfs->fs_snap = fs->fs_next;
    } else {
        nfs = pfs ? pfs->fs_snap : dfs_getGlobalFs(gfs);
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

/* Find out inode numbers for "image/dfs/layerdb/mounts" and
 * "image/dfs/layerdb/sha256" directories.
 */
void
dfs_setupSpecialDir(struct gfs *gfs, struct fs *fs) {
    struct inode *inode;
    ino_t inum;
    int i;

    if (gfs->gfs_layerdb_root) {
        return;
    }

    char *path[] = {"image", "dfs", "layerdb"};
    inum = DFS_ROOT_INODE;
    for (i = 0; i < 3; i++) {
        inode = dfs_getInode(fs, inum, NULL, false, false);
        if (inode == NULL) {
            dfs_reportError(__func__, __LINE__, inum, ENOENT);
            return;
        }
        inum = dfs_dirLookup(fs, inode, path[i]);
        dfs_inodeUnlock(inode);
        if (inum == DFS_INVALID_INODE) {
            dfs_reportError(__func__, __LINE__, inum, ENOENT);
            return;
        }
    }
    inode = dfs_getInode(fs, inum, NULL, false, false);
    if (inode == NULL) {
        dfs_reportError(__func__, __LINE__, inum, ENOENT);
        return;
    }
    gfs->gfs_layerdb_root = inum;
    printf("layerdb root %ld\n", inum);
    inum = dfs_dirLookup(fs, inode, "mounts");
    if (inum != DFS_INVALID_INODE) {
        gfs->gfs_mounts_root = inum;
        printf("mounts root %ld\n", inum);
    }
    inum = dfs_dirLookup(fs, inode, "sha256");
    if (inum != DFS_INVALID_INODE) {
        gfs->gfs_sha256_root = inum;
        printf("sha256 root %ld\n", inum);
    }
    dfs_inodeUnlock(inode);
}

/* Format a file system by initializing its super block */
static void
dfs_format(struct gfs *gfs, struct fs *fs, size_t size) {
    dfs_superInit(gfs->gfs_super, size, true);
    dfs_rootInit(fs, fs->fs_root);
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

/* Initialize a file system after reading its super block */
static struct fs *
dfs_initfs(struct gfs *gfs, struct fs *pfs, uint64_t block, bool child) {
    struct super *super = dfs_superRead(gfs, block);
    struct fs *fs;
    int i;

    fs = dfs_newFs(gfs, super->sb_flags & DFS_SUPER_RDWR, true);
    fs->fs_sblock = block;
    fs->fs_super = super;
    fs->fs_root = fs->fs_super->sb_root;
    if (child) {
        assert(pfs->fs_snap == NULL);
        pfs->fs_snap = fs;
        fs->fs_parent = pfs;
        fs->fs_ilock = pfs->fs_ilock;
    } else if (pfs->fs_parent == NULL) {
        assert(pfs->fs_next == NULL);
        pfs->fs_next = fs;
        fs->fs_ilock = malloc(sizeof(pthread_mutex_t));
        pthread_mutex_init(fs->fs_ilock, NULL);
    } else {
        assert(pfs->fs_next == NULL);
        pfs->fs_next = fs;
        fs->fs_parent = pfs->fs_parent;
        fs->fs_ilock = pfs->fs_ilock;
    }
    i = fs->fs_super->sb_index;
    assert(i < DFS_FS_MAX);
    assert(gfs->gfs_fs[i] == NULL);
    gfs->gfs_fs[i] = fs;
    gfs->gfs_roots[i] = fs->fs_root;
    if (i > gfs->gfs_scount) {
        gfs->gfs_scount = i;
    }
    fs->fs_gindex = i;
    dfs_printf("Added fs with parent %ld root %ld index %d block %ld\n",
               fs->fs_parent ? fs->fs_parent->fs_root : - 1,
               fs->fs_root, fs->fs_gindex, block);
    return fs;
}

/* Initialize all file systems from disk */
static void
dfs_initSnapshots(struct gfs *gfs, struct fs *pfs) {
    struct fs *fs, *nfs = pfs;
    uint64_t block;

    /* Initialize all snapshots of the same parent */
    block = pfs->fs_super->sb_nextSnap;
    while (block) {
        fs = dfs_initfs(gfs, nfs, block, false);
        nfs = fs;
        block = fs->fs_super->sb_nextSnap;
    }

    /* Now initialize all the child snapshots */
    nfs = pfs;
    while (nfs) {
        block = nfs->fs_super->sb_childSnap;
        if (block) {
            fs = dfs_initfs(gfs, nfs, block, true);
            dfs_initSnapshots(gfs, fs);
        }
        nfs = nfs->fs_next;
    }
}

/* Mount the device */
int
dfs_mount(char *device, struct gfs **gfsp) {
    struct gfs *gfs;
    struct fs *fs;
    size_t size;
    int fd, err;

    assert(sizeof(struct super) == DFS_BLOCK_SIZE);

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

    /* Initialize a file system structure in memory */
    fs = dfs_newFs(gfs, true, false);
    fs->fs_root = DFS_ROOT_INODE;
    fs->fs_sblock = DFS_SUPER_BLOCK;
    gfs->gfs_fs[0] = fs;
    gfs->gfs_roots[0] = DFS_ROOT_INODE;

    /* Try to find a valid superblock, if not found, format the device */
    fs->fs_super = dfs_superRead(gfs, fs->fs_sblock);
    gfs->gfs_super = fs->fs_super;
    if (gfs->gfs_super->sb_version != DFS_VERSION) {
        dfs_format(gfs, fs, size);
    } else {
        if (gfs->gfs_super->sb_flags & DFS_SUPER_DIRTY) {
            printf("Filesystem is dirty\n");
            return EIO;
        }
        gfs->gfs_super->sb_mounts++;
        err = dfs_readInodes(fs);
        if (err != 0) {
            printf("Reading inodes failed, err %d\n", err);
            return EIO;
        }
        dfs_initSnapshots(gfs, fs);
    }

    /* Write out the file system super block */
    gfs->gfs_super->sb_flags |= DFS_SUPER_DIRTY | DFS_SUPER_RDWR;
    err = dfs_superWrite(gfs, fs);
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
    struct fs *fs;
    int i;

    gfs->gfs_super->sb_flags &= ~DFS_SUPER_DIRTY;
    dfs_superWrite(gfs, dfs_getGlobalFs(gfs));
    close(gfs->gfs_fd);
    pthread_mutex_destroy(&gfs->gfs_lock);
    for (i = 0; i <= gfs->gfs_scount; i++) {
        fs = gfs->gfs_fs[i];
        if (fs) {
            dfs_destroyFs(fs);
        }
    }
    free(gfs->gfs_fs);
    free(gfs->gfs_roots);
    free(gfs);
}

/* Write out superblocks of all file systems */
void
dfs_umountAll(struct gfs *gfs) {
    struct fs *fs;
    int err;
    int i;

    for (i = 1; i <= gfs->gfs_scount; i++) {
        fs = gfs->gfs_fs[i];
        if (fs && (fs->fs_super->sb_flags & DFS_SUPER_DIRTY)) {
            fs->fs_super->sb_flags &= ~DFS_SUPER_DIRTY;
            printf("Writing out file system superblock for fs %d %ld to block %ld\n", fs->fs_gindex, fs->fs_root, fs->fs_sblock);
            err = dfs_superWrite(gfs, fs);
            if (err) {
                printf("Superblock update error %d for fs index %d root %ld\n",
                       err, fs->fs_gindex, fs->fs_root);
            }
        }
    }
}
