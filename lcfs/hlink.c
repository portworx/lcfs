#include "includes.h"

#ifdef LC_DIFF
/* Copy hardlink records from parent */
static void
lc_copyHlinks(struct fs *fs) {
    struct hldata *hldata = fs->fs_hlinks, *new, **prev = &fs->fs_hlinks;

    assert(fs->fs_sharedHlinks);
    fs->fs_hlinks = NULL;
    while (hldata) {
        new = lc_malloc(fs, sizeof(struct hldata), LC_MEMTYPE_HLDATA);
        new->hl_ino = hldata->hl_ino;
        new->hl_parent = hldata->hl_parent;
        new->hl_nlink = hldata->hl_nlink;
        new->hl_next = NULL;
        *prev = new;
        prev = &new->hl_next;
        hldata = hldata->hl_next;
    }
    fs->fs_sharedHlinks = false;
}

/* Add a new hardlink record for the inode */
void
lc_addHlink(struct fs *fs, struct inode *inode, ino_t parent) {
    ino_t ino = inode->i_ino;
    struct hldata *hldata;

    /* Hardlinks are not tracked after remount or for the root layer */
    if (fs->fs_rfs->fs_restarted || (fs == lc_getGlobalFs(fs->fs_gfs))) {
        return;
    }
    assert(!S_ISDIR(inode->i_mode));

    /* Use special notation for files in root directory */
    if (parent == fs->fs_root) {
        parent = LC_ROOT_INODE;
    }

    pthread_mutex_lock(&fs->fs_hlock);
    if (fs->fs_sharedHlinks) {
        lc_copyHlinks(fs);
    }

    /* If the current parent does not have a hardlink record, create one */
    if (!(inode->i_flags & LC_INODE_MLINKS)) {
        inode->i_flags |= LC_INODE_MLINKS;
        hldata = lc_malloc(fs, sizeof(struct hldata), LC_MEMTYPE_HLDATA);
        hldata->hl_ino = ino;
        hldata->hl_parent = (inode->i_parent == fs->fs_root) ?
                                LC_ROOT_INODE : inode->i_parent;
        hldata->hl_nlink = 1;
        hldata->hl_next = fs->fs_hlinks;
        fs->fs_hlinks = hldata;

        /* If the new link is created in the same directory, return after
         * incrementing the link count.
         */
        if (parent == hldata->hl_parent) {
            hldata->hl_nlink++;
            pthread_mutex_unlock(&fs->fs_hlock);
            return;
        }
        hldata = NULL;
    } else {

        /* Check if a hardlink record exists for this inode corresponding to
         * this parent.
         */
        hldata = fs->fs_hlinks;
        while (hldata &&
               ((hldata->hl_ino != ino) || (hldata->hl_parent != parent))) {
            hldata = hldata->hl_next;
        }
    }
    if (hldata) {

        /* Increment link count and return */
        assert(ino == hldata->hl_ino);
        assert(parent == hldata->hl_parent);
        hldata->hl_nlink++;
    } else {

        /* Create a new hardlink record */
        hldata = lc_malloc(fs, sizeof(struct hldata), LC_MEMTYPE_HLDATA);
        hldata->hl_ino = ino;
        hldata->hl_parent = parent;
        hldata->hl_nlink = 1;
        hldata->hl_next = fs->fs_hlinks;
        fs->fs_hlinks = hldata;
    }
    pthread_mutex_unlock(&fs->fs_hlock);
}

/* Remove a hardlink record for the inode */
void
lc_removeHlink(struct fs *fs, struct inode *inode, ino_t parent) {
    ino_t ino = inode->i_ino;
    struct hldata *hldata, **prev = &fs->fs_hlinks;

    assert(!fs->fs_rfs->fs_restarted);
    assert(!S_ISDIR(inode->i_mode));
    assert(inode->i_flags & LC_INODE_MLINKS);
    if (fs->fs_hlinks == NULL) {
        return;
    }
    if (parent == fs->fs_root) {
        parent = LC_ROOT_INODE;
    }

    /* Find the hardlink record */
    pthread_mutex_lock(&fs->fs_hlock);
    if (fs->fs_sharedHlinks) {
        lc_copyHlinks(fs);
    }
    hldata = fs->fs_hlinks;
    while (hldata &&
           ((hldata->hl_ino != ino) || (hldata->hl_parent != parent))) {
        prev = &hldata->hl_next;
        hldata = hldata->hl_next;
    }
    assert(ino == hldata->hl_ino);
    assert(parent == hldata->hl_parent);

    /* Decrement link count and free the record when linkcount reaches 0 */
    assert(hldata->hl_nlink > 0);
    hldata->hl_nlink--;
    if (hldata->hl_nlink == 0) {
        *prev = hldata->hl_next;
        pthread_mutex_unlock(&fs->fs_hlock);
        lc_free(fs, hldata, sizeof(struct hldata), LC_MEMTYPE_HLDATA);
    } else {
        pthread_mutex_unlock(&fs->fs_hlock);
    }
}

/* Remove hardlinks tracked by the layer */
void
lc_freeHlinks(struct fs *fs) {
    struct hldata *hldata = fs->fs_hlinks, *tmp;

    fs->fs_hlinks = NULL;
    if (fs->fs_sharedHlinks) {
        return;
    }
    while (hldata) {
        tmp = hldata;
        hldata = hldata->hl_next;
        lc_free(fs, tmp, sizeof(struct hldata), LC_MEMTYPE_HLDATA);
    }
}
#endif
