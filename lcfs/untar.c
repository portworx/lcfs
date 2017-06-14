#include "includes.h"

/* Open a tar archive */
static int
lc_archive_open(struct archive *a, void *client_data) {
    return ARCHIVE_OK;
}

/* Read next page from the archive */
static ssize_t
lc_archive_read(struct archive *a, void *client_data, const void **buffer) {
    struct inode *inode = (struct inode *)client_data;
    uint64_t size, pg = lc_inodeGetFirstPage(inode);
    uint64_t offset = (pg * LC_BLOCK_SIZE);

    if (offset >= inode->i_size) {
        return 0;
    }
    size = inode->i_size - offset;
    if (size > LC_BLOCK_SIZE) {
        size = LC_BLOCK_SIZE;
    }
    *buffer = lc_getDirtyPage(inode->i_fs->fs_gfs, inode, pg, NULL);
    lc_inodeSetFirstPage(inode, pg + 1);
    return size;
}

/* Close the archive */
static int
lc_archive_close(struct archive *a, void *client_data) {
    return ARCHIVE_OK;
}

/* Extract data of a regular file */
static void
lc_extract_data(struct fs *fs, struct inode *inode, struct archive *a) {
    uint64_t size, psize, count = 0;
    int64_t offset, off;
    struct dpage dpage;
    const char *page;
    char *pdata;
    int err;

    for (;;) {
        err = archive_read_data_block(a, (const void **)&page, &size, &offset);
        if (err == ARCHIVE_EOF) {
            break;
        }
        off = offset % LC_BLOCK_SIZE;
        psize = LC_BLOCK_SIZE - off;
        if (psize > size) {
            psize = size;
        }
        while (size) {
            lc_mallocBlockAligned(fs, (void **)&pdata, LC_MEMTYPE_DATA);
            memcpy(&pdata[off], &page[offset], psize);
            dpage.dp_data = pdata;
            dpage.dp_poffset = off;
            dpage.dp_psize = psize;
            if (lc_addPages(inode, offset, psize, &dpage, 1)) {
                count++;
            }
            lc_freePages(fs, &dpage, 1);
            offset += psize;
            size -= psize;
            off = 0;
            psize = LC_BLOCK_SIZE;
            if (psize > size) {
                psize = size;
            }
        }
    }
    lc_markInodeDirty(inode, LC_INODE_EMAPDIRTY);
    if (count) {
        __sync_add_and_fetch(&fs->fs_pcount, count);
        __sync_add_and_fetch(&fs->fs_gfs->gfs_dcount, count);
    }
}

/* Extract an entry */
static void
lc_extract_entry(struct inode *target, struct archive *a,
                 struct archive_entry *entry) {
    const char *path, *symlink, *hardlink, *name;
    struct inode *inode, *dir = target;
    struct fs *fs = target->i_fs;
    uint64_t size;
    mode_t mode;
    int i, len;
    dev_t rdev;
    ino_t ino;
    uid_t uid;
    gid_t gid;

    path = archive_entry_pathname(entry);
    mode = archive_entry_mode(entry);
    uid = archive_entry_uid(entry);
    gid = archive_entry_gid(entry);
    rdev = archive_entry_rdev(entry);
    size = archive_entry_size(entry);
    symlink = archive_entry_symlink(entry);
    hardlink = archive_entry_hardlink(entry);
    lc_printf("x %s mode 0x%x uid %d gid %d rdev %ld size %ld"
              " symlink %s hardlink %s\n",
              path, mode, uid, gid, rdev, size, symlink, hardlink);
    len = strlen(path);
    i = len - 1;
    while (i) {
        if (path[i] == '/') {
            if (i < (len - 1)) {
                i++;
                break;
            }
            len--;
        }
        i--;
    }
    if (i) {
        dir = lc_pathLookup(fs, dir, path, i - 1);
    }
    name = &path[i];
    if (hardlink) {
        inode = lc_pathLookup(fs, target, hardlink, strlen(hardlink));
        lc_inodeLock(inode, true);
        inode->i_nlink++;
    } else {
        inode = lc_inodeInit(fs, mode, uid, gid, rdev, dir->i_ino, symlink);
    }
    ino = inode->i_ino;
    lc_inodeLock(dir, true);
    lc_dirAdd(dir, ino, mode, name, len - i);
    if (S_ISDIR(mode)) {
        dir->i_nlink++;
    }
    lc_updateInodeTimes(dir, true, true);
    lc_markInodeDirty(dir, LC_INODE_DIRDIRTY);
    lc_inodeUnlock(dir);
    if (S_ISREG(mode) && size) {
        inode->i_size = size;
        lc_extract_data(fs, inode, a);
    }

    /* XXX Extract ACLs and extended attributes */
    lc_markInodeDirty(inode, 0);
    lc_inodeUnlock(inode);
}

/* Extract an archive */
void
lc_extract(struct inode *inode, struct inode *target) {
    struct archive_entry *entry;
    struct archive *a;
    uint64_t pg;
    int err;

    lc_printf("Extracting archive inode %ld\n", inode->i_ino);
    assert(S_ISREG(inode->i_mode));
    assert(inode->i_flags & LC_INODE_TMP);
    assert(S_ISDIR(target->i_mode));
    lc_inodeLock(inode, true);
    pg = lc_inodeGetFirstPage(inode);
    assert(pg == 0);
    a = archive_read_new();
    archive_read_support_format_tar(a);
    archive_read_support_filter_gzip(a);

    err = archive_read_open(a, inode, lc_archive_open, lc_archive_read,
                            lc_archive_close);
    assert(err == ARCHIVE_OK);
    for (;;) {
        err = archive_read_next_header(a, &entry);
        if (err == ARCHIVE_EOF) {
            break;
        }
        assert(err == ARCHIVE_OK);
        lc_extract_entry(target, a, entry);
    }
    archive_read_close(a);
    archive_read_free(a);
    lc_inodeSetFirstPage(inode, pg);
    lc_inodeUnlock(inode);
}
