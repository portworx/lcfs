#ifndef _INCLUDE_H
#define _INCLUDE_H

/* This file has function prototypes */

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>

#include "inlines.h"
#include "layout.h"
#include "fs.h"
#include "inode.h"

void *dfs_readBlock(int fd, off_t block);
int dfs_writeBlock(int fd, void *buf, off_t block);

int dfs_superRead(struct gfs *gfs);
int dfs_superWrite(struct gfs *gfs);

struct gfs *getfs();
struct fs *dfs_getfs(struct gfs *gfs, ino_t root);
struct fs *dfs_checkfs(struct fs *fs, ino_t ino);
void dfs_format(struct gfs *gfs, size_t size);
void dfs_lock(struct gfs *gfs, bool exclusive);
void dfs_unlock(struct gfs *gfs);

int dfs_readInodes(struct fs *fs);
struct inode *dfs_getInode(struct fs *fs, ino_t ino, bool copy,
                           bool exclusive);
struct inode *dfs_getPathInode(const char *path, struct gfs *gfs, bool copy,
                               bool exclusive);
ino_t dfs_inodeInit(struct fs *fs, mode_t mode,
                    uid_t uid, gid_t gid, dev_t rdev, const char *target);
void dfs_updateInodeTimes(struct inode *inode, bool atime,
                          bool mtime, bool ctime);
void dfs_inodeLock(struct inode *inode, bool exclusive);
void dfs_inodeUnlock(struct inode *inode);

ino_t dfs_dirLookup(struct fs *fs, ino_t ino, const char *name);
ino_t dfs_lookup(const char *path, struct gfs *gfs, struct fs **fsp,
                 ino_t *dir, char *fname);
void dfs_dirAdd(struct inode *dir, ino_t ino, mode_t mode, char *name);
void dfs_dirRemove(struct inode *dir, char *name);
void dfs_dirRename(struct inode *dir, ino_t ino, char *name);
void dfs_dirCopy(struct inode *inode, struct inode *dir);

void dfs_addPage(struct inode *inode, uint64_t pg, off_t poffset, size_t psize,
                 const char *buf);
void dfs_readPages(struct inode *inode, off_t soffset, off_t endoffset,
                   char *buf);

int dfs_newClone(ino_t ino, ino_t pino, const char *name);
int dfs_removeClone(const char *path);

#endif
