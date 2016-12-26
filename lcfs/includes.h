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
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <pthread.h>
#include <assert.h>
#include <linux/ioctl.h>

#include "lcfs.h"
#include "layout.h"
#include "memory.h"
#include "inode.h"
#include "fs.h"
#include "extent.h"
#include "page.h"
#include "stats.h"
#include "inlines.h"

struct gfs *getfs();

void lc_memoryInit(void);
void *lc_malloc(struct fs *fs, size_t size, enum lc_memTypes type);
void lc_mallocBlockAligned(struct fs *fs, void **memptr, enum lc_memTypes type);
void lc_free(struct fs *fs, void *ptr, size_t size, enum lc_memTypes type);
bool lc_checkMemoryAvailable(void);
void lc_waitMemory(void);
void lc_memUpdateTotal(struct fs *fs, size_t size);
void lc_memTransferCount(struct fs *fs, uint64_t count);
void lc_checkMemStats(struct fs *fs);
void lc_displayGlobalMemStats();
void lc_displayMemStats(struct fs *fs);

void lc_readBlock(struct gfs *gfs, struct fs *fs, off_t block, void *dbuf);
int lc_writeBlock(struct gfs *gfs, struct fs *fs, void *buf, off_t block);
int lc_writeBlocks(struct gfs *gfs, struct fs *fs,
                    struct iovec *iov, int iovcnt, off_t block);

void lc_addExtent(struct gfs *gfs, struct fs *fs, struct extent **extents,
                  uint64_t start, uint64_t block, uint64_t count);
uint64_t lc_removeExtent(struct fs *fs, struct extent **extents,
                         uint64_t start, uint64_t count);
void lc_freeExtent(struct gfs *gfs, struct fs *fs, struct extent *extent,
                   struct extent *prev, struct extent **extents, bool layer);

void lc_blockAllocatorInit(struct gfs *gfs, struct fs *fs);
void lc_blockAllocatorDeinit(struct gfs *gfs, struct fs *fs);
bool lc_hasSpace(struct gfs *gfs, uint64_t blocks);
void lc_addSpaceExtent(struct gfs *gfs, struct fs *fs, struct extent **extents,
                       uint64_t start, uint64_t count);
uint64_t lc_freeLayerBlocks(struct gfs *gfs, struct fs *fs, bool unmount,
                            bool remove);
uint64_t lc_blockAlloc(struct fs *fs, uint64_t count, bool meta, bool reserve);
uint64_t lc_blockAllocExact(struct fs *fs, uint64_t count,
                            bool meta, bool reserve);
void lc_blockFree(struct gfs *gfs, struct fs *fs, uint64_t block,
                  uint64_t count, bool layer);
void lc_freeLayerMetaBlocks(struct fs *fs, uint64_t block, uint64_t count);
void lc_freeLayerDataBlocks(struct fs *fs, uint64_t block, uint64_t count,
                            bool allocated);
void lc_processFreedBlocks(struct fs *fs, bool remove);
uint64_t lc_blockFreeExtents(struct fs *fs, struct extent *extents,
                             bool efree, bool flush, bool layer);
void lc_replaceMetaBlocks(struct fs *fs, struct extent **extents,
                          uint64_t block, uint64_t count);
void lc_readExtents(struct gfs *gfs, struct fs *fs);
void lc_displayAllocStats(struct fs *fs);

void lc_superRead(struct gfs *gfs, struct fs *fs, uint64_t block);
int lc_superWrite(struct gfs *gfs, struct fs *fs);
void lc_superInit(struct super *super, size_t size, bool global);

struct fs *lc_getfs(ino_t ino, bool exclusive);
uint64_t lc_getfsForRemoval(struct gfs *gfs, ino_t root, struct fs **fsp);
int lc_getIndex(struct fs *nfs, ino_t parent, ino_t ino);
void lc_addfs(struct gfs *gfs, struct fs *fs, struct fs *pfs);
void lc_removefs(struct gfs *gfs, struct fs *fs);
void lc_lock(struct fs *fs, bool exclusive);
int lc_tryLock(struct fs *fs, bool exclusive);
void lc_unlock(struct fs *fs);
int lc_mount(char *device, struct gfs **gfsp);
void lc_newInodeBlock(struct gfs *gfs, struct fs *fs);
void lc_flushInodeBlocks(struct gfs *gfs, struct fs *fs);
void lc_invalidateInodeBlocks(struct gfs *gfs, struct fs *fs);
void lc_sync(struct gfs *gfs, struct fs *fs, bool super);
void lc_unmount(struct gfs *gfs);
void lc_syncAllLayers(struct gfs *gfs);
struct fs *lc_newFs(struct gfs *gfs, size_t icsize, bool rw);
void lc_destroyFs(struct fs *fs, bool remove);

void lc_icache_init(struct fs *fs, size_t size);
void lc_icache_deinit(struct icache *icache);
ino_t lc_inodeAlloc(struct fs *fs);
void lc_updateFtypeStats(struct fs *fs, mode_t mode, bool incr);
void lc_displayFtypeStats(struct fs *fs);
int lc_readInodes(struct gfs *gfs, struct fs *fs);
void lc_destroyInodes(struct fs *fs, bool remove);
struct inode *lc_getInode(struct fs *fs, ino_t ino, struct inode *handle,
                           bool copy, bool exclusive);
struct inode *lc_inodeInit(struct fs *fs, mode_t mode,
                            uid_t uid, gid_t gid, dev_t rdev, ino_t parent,
                            const char *target);
void lc_rootInit(struct fs *fs, ino_t root);
void lc_cloneRootDir(struct inode *pdir, struct inode *dir);
void lc_setSnapshotRoot(struct gfs *gfs, ino_t ino);
void lc_updateInodeTimes(struct inode *inode, bool mtime, bool ctime);
void lc_syncInodes(struct gfs *gfs, struct fs *fs);
void lc_inodeLock(struct inode *inode, bool exclusive);
void lc_inodeUnlock(struct inode *inode);
int lc_flushInode(struct gfs *gfs, struct fs *fs, struct inode *inode);
void lc_invalidateInodePages(struct gfs *gfs, struct fs *fs);

ino_t lc_dirLookup(struct fs *fs, struct inode *dir, const char *name);
void lc_dirAdd(struct inode *dir, ino_t ino, mode_t mode, const char *name,
               int nsize);
void lc_dirReaddir(fuse_req_t req, struct fs *fs, struct inode *dir,
                   fuse_ino_t ino, size_t size, off_t off, struct stat *st);
void lc_dirRemove(struct inode *dir, const char *name);
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
void  lc_dirConvertHashed(struct fs *fs, struct inode *dir);
void lc_dirFreeHash(struct fs *fs, struct inode *dir);
void lc_dirFree(struct inode *dir);

uint64_t lc_inodeEmapLookup(struct gfs *gfs, struct inode *inode,
                            uint64_t page, struct extent **extents);
void lc_copyEmap(struct gfs *gfs, struct fs *fs, struct inode *inode);
void lc_expandEmap(struct gfs *gfs, struct fs *fs, struct inode *inode);
void lc_inodeEmapUpdate(struct gfs *gfs, struct fs *fs, struct inode *inode,
                        uint64_t pstart, uint64_t bstart, uint64_t pcount,
                        struct extent **extents);
void lc_emapFlush(struct gfs *gfs, struct fs *fs, struct inode *inode);
void lc_emapRead(struct gfs *gfs, struct fs *fs, struct inode *inode,
                 void *buf);
void lc_emapTruncate(struct gfs *gfs, struct fs *fs, struct inode *inode,
                     size_t size, uint64_t pg, bool remove, bool *truncated);
void lc_freeInodeDataBlocks(struct fs *fs, struct inode *inode,
                            struct extent **extents);

void lc_pcache_init(struct fs *fs, uint32_t count, uint32_t lcount);
void lc_destroy_pages(struct gfs *gfs, struct fs *fs, struct pcache *pcache,
                      bool remove);
struct page *lc_getPage(struct fs *fs, uint64_t block, bool read);
struct page *lc_getPageNoBlock(struct gfs *gfs, struct fs *fs, char *data,
                               struct page *prev);
struct page *lc_getPageNew(struct gfs *gfs, struct fs *fs,
                           uint64_t block, char *data);
void lc_releasePage(struct gfs *gfs, struct fs *fs, struct page *page,
                    bool read);
void lc_releaseReadPages(struct gfs *gfs, struct fs *fs,
                         struct page **pages, uint64_t pcount);
struct page *lc_getPageNewData(struct fs *fs, uint64_t block);
void lc_addPageBlockHash(struct gfs *gfs, struct fs *fs,
                         struct page *page, uint64_t block);
void lc_flushPageCluster(struct gfs *gfs, struct fs *fs,
                         struct page *head, uint64_t count, bool bfree);
void lc_releasePages(struct gfs *gfs, struct fs *fs, struct page *head);
void lc_addPageForWriteBack(struct gfs *gfs, struct fs *fs, struct page *head,
                            struct page *tail, uint64_t pcount);
void *lc_flusher(void *data);

uint64_t lc_copyPages(struct fs *fs, off_t off, size_t size,
                      struct dpage *dpages, struct fuse_bufvec *bufv,
                      struct fuse_bufvec *dst);
uint64_t lc_addPages(struct inode *inode, off_t off, size_t size,
                     struct dpage *dpages, uint64_t pcount);
void lc_readPages(fuse_req_t req, struct inode *inode, off_t soffset,
                  off_t endoffset, struct page **pages,
                  struct fuse_bufvec *bufv);
void lc_flushPages(struct gfs *gfs, struct fs *fs, struct inode *inode,
                   bool release, bool unlock);
void lc_truncatePage(struct fs *fs, struct inode *inode, struct dpage *dpage,
                     uint64_t pg, uint16_t poffset);
void lc_truncPages(struct inode *inode, off_t size, bool remove);
void lc_flushDirtyPages(struct gfs *gfs, struct fs *fs);
void lc_addDirtyInode(struct fs *fs, struct inode *inode);
void lc_flushDirtyInodeList(struct fs *fs, bool force);
void lc_invalidateDirtyPages(struct gfs *gfs, struct fs *fs);
void lc_purgePages(struct gfs *gfs, bool force);
bool lc_flushInodeDirtyPages(struct inode *inode, uint64_t page, bool unlock,
                             bool force);

int lc_removeInode(struct fs *fs, struct inode *dir, ino_t ino, bool rmdir,
                   void **fsp);

void lc_xattrAdd(fuse_req_t req, ino_t ino, const char *name,
                  const char *value, size_t size, int flags);
void lc_xattrGet(fuse_req_t req, ino_t ino, const char *name, size_t size);
void lc_xattrList(fuse_req_t req, ino_t ino, size_t size);
void lc_xattrRemove(fuse_req_t req, ino_t ino, const char *name);
bool lc_xattrCopy(struct inode *inode, struct inode *parent);
void lc_xattrFlush(struct gfs *gfs, struct fs *fs, struct inode *inode);
void lc_xattrRead(struct gfs *gfs, struct fs *fs, struct inode *inode,
                  void *buf);
void lc_xattrFree(struct inode *inode);

void lc_linkParent(struct fs *fs, struct fs *pfs);
void lc_newClone(fuse_req_t req, struct gfs *gfs, const char *name,
                  const char *parent, size_t size, bool rw);
void lc_removeClone(fuse_req_t req, struct gfs *gfs, const char *name);
void lc_snapIoctl(fuse_req_t req, struct gfs *gfs, const char *name,
                  enum ioctl_cmd cmd);

void lc_statsNew(struct fs *fs);
void lc_statsBegin(struct timeval *start);
void lc_statsAdd(struct fs *fs, enum lc_stats type, bool err,
                  struct timeval *start);
void lc_displayLayerStats(struct fs *fs);
void lc_displayStats(struct fs *fs);
void lc_displayStatsAll(struct gfs *gfs);
void lc_displayGlobalStats(struct gfs *gfs);
void lc_statsDeinit(struct fs *fs);

#endif
