#ifndef _INCLUDE_H
#define _INCLUDE_H

#define FUSE_USE_VERSION 29

#define _GNU_SOURCE

#include <fuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <pthread.h>
#include <assert.h>

#include "inlines.h"
#include "layout.h"
#include "fs.h"
#include "inode.h"
#include "stats.h"

struct gfs *getfs();

void *dfs_readBlock(int fd, off_t block);
int dfs_writeBlock(int fd, void *buf, off_t block);
int dfs_writeBlocks(int fd, struct iovec *iov, int iovcnt, off_t block);

void dfs_blockAlloc(struct fs *fs, int count);
void dfs_blockFree(struct gfs *gfs, uint64_t count);

int dfs_superRead(struct gfs *gfs);
int dfs_superWrite(struct gfs *gfs);

struct fs *dfs_getfs(ino_t ino, bool exclusive);
int dfs_getIndex(struct fs *nfs, ino_t parent, ino_t ino);
void dfs_setupSpecialDir(struct gfs *gfs, struct fs *fs);
void dfs_addfs(struct fs *fs, struct fs *snap);
void dfs_removefs(struct gfs *gfs, struct fs *fs);
void dfs_removeSnap(struct gfs *gfs, struct fs *fs);
void dfs_format(struct gfs *gfs, size_t size);
void dfs_lock(struct fs *fs, bool exclusive);
void dfs_unlock(struct fs *fs);
int dfs_mount(char *device, struct gfs **gfsp);
void dfs_unmount(struct gfs *gfs);
void dfs_umount(const char *name);
struct fs *dfs_newFs(struct gfs *gfs, bool rw, bool locks);
void dfs_destroyFs(struct fs *fs);

struct icache *dfs_icache_init();
void dfs_icache_deinit(struct icache *icache);
ino_t dfs_inodeAlloc(struct fs *fs);
int dfs_readInodes(struct fs *fs);
uint64_t dfs_destroyInodes(struct fs *fs);
struct inode *dfs_getInode(struct fs *fs, ino_t ino, struct inode *handle,
                           bool copy, bool exclusive);
struct inode *dfs_inodeInit(struct fs *fs, mode_t mode,
                            uid_t uid, gid_t gid, dev_t rdev, ino_t parent,
                            const char *target);
void dfs_updateInodeTimes(struct inode *inode, bool atime,
                          bool mtime, bool ctime);
void dfs_inodeLock(struct inode *inode, bool exclusive);
void dfs_inodeUnlock(struct inode *inode);
void dfs_invalidate_pcache(struct gfs *gfs, struct fs *fs);

ino_t dfs_dirLookup(struct fs *fs, struct inode *dir, const char *name);
void dfs_dirAdd(struct inode *dir, ino_t ino, mode_t mode, const char *name);
void dfs_dirRemove(struct inode *dir, const char *name);
void dfs_dirRemoveInode(struct inode *dir, ino_t ino);
void dfs_dirRename(struct inode *dir, ino_t ino,
                   const char *name, const char *newname);
void dfs_dirCopy(struct inode *inode, struct inode *dir);
void dfs_removeTree(struct fs *fs, struct inode *dir);
void dfs_dirFree(struct inode *dir);

int dfs_addPages(struct inode *inode, off_t off, size_t size,
                 struct fuse_bufvec *bufv, struct fuse_bufvec *dst);
void dfs_readPages(struct inode *inode, off_t soffset, off_t endoffset,
                   struct fuse_bufvec *bufv);
uint64_t dfs_truncPages(struct inode *inode, off_t size);
void dfs_inodeAllocPages(struct inode *inode);

int dremove(struct fs *fs, struct inode *dir, const char *name,
            ino_t ino, bool rmdir);

void dfs_xattrAdd(fuse_req_t req, ino_t ino, const char *name,
                  const char *value, size_t size, int flags);
void dfs_xattrGet(fuse_req_t req, ino_t ino, const char *name, size_t size);
void dfs_xattrList(fuse_req_t req, ino_t ino, size_t size);
void dfs_xattrRemove(fuse_req_t req, ino_t ino, const char *name);
void dfs_xattrCopy(struct inode *inode, struct inode *parent);
void dfs_xattrFree(struct inode *inode);

ino_t dfs_getRootIno(struct fs *fs, ino_t parent, const char *name);
void dfs_newClone(fuse_req_t req, struct gfs *gfs, const char *name,
                  const char *parent, size_t size, bool rw);
void dfs_removeClone(fuse_req_t req, struct gfs *gfs,
                     ino_t ino, const char *name);

struct stats *dfs_statsNew();
void dfs_statsBegin(struct timeval *start);
void dfs_statsAdd(struct fs *fs, enum dfs_stats type, bool err,
                  struct timeval *start);
void dfs_displayStats(struct fs *fs);
void dfs_displayStatsAll(struct gfs *gfs);
void dfs_statsDeinit(struct fs *fs);

#endif
