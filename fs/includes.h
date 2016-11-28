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
#include <linux/ioctl.h>

#include "lcfs.h"
#include "layout.h"
#include "inode.h"
#include "fs.h"
#include "block.h"
#include "page.h"
#include "stats.h"
#include "inlines.h"

struct gfs *getfs();

void *lc_readBlock(struct gfs *gfs, struct fs *fs, off_t block, void *dbuf);
int lc_writeBlock(struct gfs *gfs, struct fs *fs, void *buf, off_t block);
int lc_writeBlocks(struct gfs *gfs, struct fs *fs,
                    struct iovec *iov, int iovcnt, off_t block);

void lc_blockAllocatorInit(struct gfs *gfs);
void lc_blockAllocatorDeinit(struct gfs *gfs, struct fs *fs);
void lc_addExtent(struct gfs *gfs, struct extent **extents,
                  uint64_t block, uint64_t count);
void lc_freeLayerBlocks(struct gfs *gfs, struct fs *fs, bool remove);
uint64_t lc_blockAlloc(struct fs *fs, uint64_t count, bool meta);
void lc_blockFree(struct fs *fs, uint64_t block, uint64_t count);
void lc_freeLayerMetaBlocks(struct fs *fs, uint64_t block, uint64_t count);
void lc_processFreedMetaBlocks(struct fs *fs, bool remove);
uint64_t lc_blockFreeExtents(struct fs *fs, struct extent *extents,
                         bool efree, bool flush);
void lc_replaceMetaBlocks(struct fs *fs, struct extent **extents,
                          uint64_t block, uint64_t count);
void lc_readExtents(struct gfs *gfs, struct fs *fs);

struct super *lc_superRead(struct gfs *gfs, uint64_t block);
int lc_superWrite(struct gfs *gfs, struct fs *fs);
void lc_superInit(struct super *super, size_t size, bool global);

struct fs *lc_getfs(ino_t ino, bool exclusive);
int lc_getIndex(struct fs *nfs, ino_t parent, ino_t ino);
void lc_addfs(struct fs *fs, struct fs *pfs);
void lc_removefs(struct gfs *gfs, struct fs *fs);
void lc_removeSnap(struct gfs *gfs, struct fs *fs);
void lc_lock(struct fs *fs, bool exclusive);
void lc_unlock(struct fs *fs);
int lc_mount(char *device, struct gfs **gfsp);
void lc_newInodeBlock(struct gfs *gfs, struct fs *fs);
void lc_flushInodeBlocks(struct gfs *gfs, struct fs *fs);
void lc_invalidateInodeBlocks(struct gfs *gfs, struct fs *fs);
void lc_sync(struct gfs *gfs, struct fs *fs);
void lc_unmount(struct gfs *gfs);
void lc_umountAll(struct gfs *gfs);
struct fs *lc_newFs(struct gfs *gfs, bool rw);
void lc_destroyFs(struct fs *fs, bool remove);

struct icache *lc_icache_init();
void lc_icache_deinit(struct icache *icache);
ino_t lc_inodeAlloc(struct fs *fs);
int lc_readInodes(struct gfs *gfs, struct fs *fs);
void lc_destroyInodes(struct fs *fs, bool remove);
struct inode *lc_getInode(struct fs *fs, ino_t ino, struct inode *handle,
                           bool copy, bool exclusive);
struct inode *lc_inodeInit(struct fs *fs, mode_t mode,
                            uid_t uid, gid_t gid, dev_t rdev, ino_t parent,
                            const char *target);
void lc_rootInit(struct fs *fs, ino_t root);
void lc_setSnapshotRoot(struct gfs *gfs, ino_t ino);
void lc_updateInodeTimes(struct inode *inode, bool atime,
                          bool mtime, bool ctime);
void lc_syncInodes(struct gfs *gfs, struct fs *fs);
void lc_inodeLock(struct inode *inode, bool exclusive);
void lc_inodeUnlock(struct inode *inode);
int lc_flushInode(struct gfs *gfs, struct fs *fs, struct inode *inode);
void lc_invalidateInodePages(struct gfs *gfs, struct fs *fs);

ino_t lc_dirLookup(struct fs *fs, struct inode *dir, const char *name);
void lc_dirAdd(struct inode *dir, ino_t ino, mode_t mode, const char *name,
                int nsize);
void lc_dirRemove(struct inode *dir, const char *name);
void lc_dirRemoveInode(struct inode *dir, ino_t ino);
void lc_dirRename(struct inode *dir, ino_t ino,
                   const char *name, const char *newname);
void lc_dirCopy(struct inode *dir);
void lc_dirRead(struct gfs *gfs, struct fs *fs, struct inode *dir, void *buf);
void lc_dirFlush(struct gfs *gfs, struct fs *fs, struct inode *dir);
void lc_removeTree(struct fs *fs, struct inode *dir);
int lc_dirRemoveName(struct fs *fs, struct inode *dir,
                     const char *name, bool rmdir, void **fsp,
                     int dremove(struct fs *, struct inode *, ino_t,
                                 bool, void **));
void lc_dirFree(struct inode *dir);

uint64_t lc_inodeBmapLookup(struct inode *inode, uint64_t page);
void lc_copyBmap(struct inode *inode);
void lc_expandBmap(struct inode *inode);
void lc_inodeBmapAlloc(struct inode *inode);
void lc_inodeBmapAdd(struct inode *inode, uint64_t page, uint64_t block);

struct pcache *lc_pcache_init();
void lc_destroy_pages(struct gfs *gfs, struct pcache *pcache, bool remove);
struct page *lc_getPageNoBlock(struct gfs *gfs, struct fs *fs, char *data,
                               struct page *prev);
struct page *lc_getPageNewData(struct fs *fs, uint64_t block);
void lc_addPageBlockHash(struct gfs *gfs, struct fs *fs,
                         struct page *page, uint64_t block);
uint64_t lc_copyPages(off_t off, size_t size, struct dpage *dpages,
                      struct fuse_bufvec *bufv, struct fuse_bufvec *dst);
uint64_t lc_addPages(struct inode *inode, off_t off, size_t size,
                     struct dpage *dpages, uint64_t pcount);
void lc_readPages(fuse_req_t req, struct inode *inode, off_t soffset,
                  off_t endoffset, struct page **pages,
                  struct fuse_bufvec *bufv);
void lc_flushPageCluster(struct gfs *gfs, struct fs *fs,
                         struct page *head, uint64_t count);
void lc_flushPages(struct gfs *gfs, struct fs *fs, struct inode *inode);
void lc_bmapFlush(struct gfs *gfs, struct fs *fs, struct inode *inode);
void lc_bmapRead(struct gfs *gfs, struct fs *fs, struct inode *inode,
                 void *buf);
void lc_truncPages(struct inode *inode, off_t size, bool remove);
void lc_flushDirtyPages(struct gfs *gfs, struct fs *fs);
void lc_invalidateDirtyPages(struct gfs *gfs, struct fs *fs);
void lc_releasePages(struct gfs *gfs, struct fs *fs, struct page *head);

int lc_removeInode(struct fs *fs, struct inode *dir, ino_t ino, bool rmdir,
               void **fsp);

void lc_xattrAdd(fuse_req_t req, ino_t ino, const char *name,
                  const char *value, size_t size, int flags);
void lc_xattrGet(fuse_req_t req, ino_t ino, const char *name, size_t size);
void lc_xattrList(fuse_req_t req, ino_t ino, size_t size);
void lc_xattrRemove(fuse_req_t req, ino_t ino, const char *name);
void lc_xattrCopy(struct inode *inode, struct inode *parent);
void lc_xattrFlush(struct gfs *gfs, struct fs *fs, struct inode *inode);
void lc_xattrRead(struct gfs *gfs, struct fs *fs, struct inode *inode,
                   void *buf);
void lc_xattrFree(struct inode *inode);

void lc_newClone(fuse_req_t req, struct gfs *gfs, const char *name,
                  const char *parent, size_t size, bool rw);
void lc_removeClone(fuse_req_t req, struct gfs *gfs, const char *name);
void lc_snapIoctl(fuse_req_t req, struct gfs *gfs, const char *name,
                  enum ioctl_cmd cmd);

struct stats *lc_statsNew();
void lc_statsBegin(struct timeval *start);
void lc_statsAdd(struct fs *fs, enum lc_stats type, bool err,
                  struct timeval *start);
void lc_displayStats(struct fs *fs);
void lc_displayStatsAll(struct gfs *gfs);
void lc_displayGlobalStats(struct gfs *gfs);
void lc_statsDeinit(struct fs *fs);

#endif
