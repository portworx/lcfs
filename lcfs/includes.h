#ifndef _INCLUDE_H
#define _INCLUDE_H

//#define FUSE3
#ifdef FUSE3
#define FUSE_USE_VERSION 30
#else
#define FUSE_USE_VERSION 29
#endif

#define LC_FTYPE_ENABLE
#define LC_STATS_ENABLE
#define LC_MEMSTATS_ENABLE
//#define LC_PROFILING

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
#include <zlib.h>
#include <assert.h>
#include <linux/ioctl.h>

#ifdef LC_PROFILING
#include <gperftools/profiler.h>
#endif

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
void lc_mallocBlockAligned(struct fs *fs, void **memptr,
                           enum lc_memTypes type);
void lc_free(struct fs *fs, void *ptr, size_t size, enum lc_memTypes type);
bool lc_checkMemoryAvailable(bool flush);
void lc_waitMemory(bool wait);
void lc_memUpdateTotal(struct fs *fs, size_t size);
void lc_memTransferCount(struct fs *fs, uint64_t count);
void lc_checkMemStats(struct fs *fs, bool unmount);
void lc_displayGlobalMemStats();
void lc_displayMemStats(struct fs *fs);

void lc_readBlock(struct gfs *gfs, struct fs *fs, off_t block, void *dbuf);
void lc_readBlocks(struct gfs *gfs, struct fs *fs, struct iovec *iov,
                   int iovcnt, off_t block);
void lc_writeBlock(struct gfs *gfs, struct fs *fs, void *buf, off_t block);
void lc_writeBlocks(struct gfs *gfs, struct fs *fs,
                    struct iovec *iov, int iovcnt, off_t block);
void lc_updateCRC(void *buf, uint32_t *crc);
void lc_verifyBlock(void *buf, uint32_t *crc);

void lc_addExtent(struct gfs *gfs, struct fs *fs, struct extent **extents,
                  uint64_t start, uint64_t block, uint64_t count, bool sort);
uint64_t lc_removeExtent(struct fs *fs, struct extent **extents,
                         uint64_t start, uint64_t count);
void lc_freeExtent(struct gfs *gfs, struct fs *fs, struct extent *extent,
                   struct extent **prev, bool layer);

void lc_blockAllocatorInit(struct gfs *gfs, struct fs *fs);
void lc_blockAllocatorDeinit(struct gfs *gfs, struct fs *fs);
bool lc_hasSpace(struct gfs *gfs, bool layer);
void lc_addSpaceExtent(struct gfs *gfs, struct fs *fs, struct extent **extents,
                       uint64_t start, uint64_t count, bool sort);
uint64_t lc_freeLayerBlocks(struct gfs *gfs, struct fs *fs, bool unmount,
                            bool remove, bool inval);
uint64_t lc_blockAlloc(struct fs *fs, uint64_t count, bool meta, bool reserve);
uint64_t lc_blockAllocExact(struct fs *fs, uint64_t count,
                            bool meta, bool reserve);
void lc_blockFree(struct gfs *gfs, struct fs *fs, uint64_t block,
                  uint64_t count, bool layer);
void lc_freeLayerMetaBlocks(struct fs *fs, uint64_t block, uint64_t count);
void lc_freeLayerDataBlocks(struct fs *fs, uint64_t block, uint64_t count,
                            bool allocated);
void lc_processFreedBlocks(struct fs *fs, bool release);
uint64_t lc_blockFreeExtents(struct fs *fs, struct extent *extents,
                             uint8_t flags);
void lc_replaceMetaBlocks(struct fs *fs, struct extent **extents,
                          uint64_t block, uint64_t count);
void lc_readExtents(struct gfs *gfs, struct fs *fs);
void lc_displayAllocStats(struct fs *fs);

bool lc_superValid(struct super *super);
void lc_superRead(struct gfs *gfs, struct fs *fs, uint64_t block);
void lc_superWrite(struct gfs *gfs, struct fs *fs);
void lc_superInit(struct super *super, uint64_t root, size_t size,
                  uint32_t flags, bool global);

struct fs *lc_getfs(ino_t ino, bool exclusive);
uint64_t lc_getfsForRemoval(struct gfs *gfs, ino_t root, struct fs **fsp);
int lc_getIndex(struct fs *nfs, ino_t parent, ino_t ino);
int lc_addfs(struct gfs *gfs, struct fs *fs, struct fs *pfs, bool *inval);
void lc_removefs(struct gfs *gfs, struct fs *fs);
void lc_lock(struct fs *fs, bool exclusive);
int lc_tryLock(struct fs *fs, bool exclusive);
void lc_unlock(struct fs *fs);
void lc_mount(struct gfs *gfs, char *device, size_t size);
void lc_newInodeBlock(struct gfs *gfs, struct fs *fs);
void lc_flushInodeBlocks(struct gfs *gfs, struct fs *fs);
void lc_invalidateInodeBlocks(struct gfs *gfs, struct fs *fs);
void lc_sync(struct gfs *gfs, struct fs *fs, bool super);
void lc_unmount(struct gfs *gfs);
void lc_syncAllLayers(struct gfs *gfs);
struct fs *lc_newFs(struct gfs *gfs, bool rw);
void lc_destroyFs(struct fs *fs, bool remove);

void lc_icache_init(struct fs *fs, size_t size);
void lc_icache_deinit(struct icache *icache);
void lc_copyStat(struct stat *st, struct inode *inode);
ino_t lc_inodeAlloc(struct fs *fs);
void lc_updateFtypeStats(struct fs *fs, mode_t mode, bool incr);
void lc_displayFtypeStats(struct fs *fs);
void lc_readInodes(struct gfs *gfs, struct fs *fs);
void lc_destroyInodes(struct fs *fs, bool remove);
struct inode *lc_lookupInode(struct fs *fs, ino_t ino);
struct inode *lc_getInode(struct fs *fs, ino_t ino, struct inode *handle,
                           bool copy, bool exclusive);
struct inode *lc_inodeInit(struct fs *fs, mode_t mode,
                            uid_t uid, gid_t gid, dev_t rdev, ino_t parent,
                            const char *target);
void lc_rootInit(struct fs *fs, ino_t root);
void lc_cloneRootDir(struct inode *pdir, struct inode *dir);
void lc_setLayerRoot(struct gfs *gfs, ino_t ino);
void lc_updateInodeTimes(struct inode *inode, bool mtime, bool ctime);
void lc_syncInodes(struct gfs *gfs, struct fs *fs);
void lc_inodeLock(struct inode *inode, bool exclusive);
void lc_inodeUnlock(struct inode *inode);
int lc_flushInode(struct gfs *gfs, struct fs *fs, struct inode *inode);
void lc_invalidateInodePages(struct gfs *gfs, struct fs *fs);
void lc_invalidateLayerPages(struct gfs *gfs, struct fs *fs);

ino_t lc_dirLookup(struct fs *fs, struct inode *dir, const char *name);
void lc_dirAdd(struct inode *dir, ino_t ino, mode_t mode, const char *name,
               int nsize);
int lc_dirReaddir(fuse_req_t req, struct fs *fs, struct inode *dir,
                  ino_t parent, size_t size, off_t off, struct stat *st);
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
bool lc_emapTruncate(struct gfs *gfs, struct fs *fs, struct inode *inode,
                     size_t size, uint64_t pg, bool remove);
void lc_freeInodeDataBlocks(struct fs *fs, struct inode *inode,
                            struct extent **extents);

void lc_bcacheInit(struct fs *fs, uint32_t count, uint32_t lcount);
void lc_bcacheFree(struct fs *fs);
void lc_destroyPages(struct gfs *gfs, struct fs *fs, bool remove);
struct page *lc_getPage(struct fs *fs, uint64_t block, bool read);
struct page *lc_getPageNoBlock(struct gfs *gfs, struct fs *fs, char *data,
                               struct page *prev);
struct page *lc_getPageNew(struct gfs *gfs, struct fs *fs,
                           uint64_t block, char *data);
void lc_readPages(struct gfs *gfs, struct fs *fs, struct page **pages,
                  uint32_t count);
void lc_releasePage(struct gfs *gfs, struct fs *fs, struct page *page,
                    bool read);
void lc_releaseReadPages(struct gfs *gfs, struct fs *fs,
                         struct page **pages, uint64_t pcount, bool nocache);
int lc_invalPage(struct gfs *gfs, struct fs *fs, uint64_t block);
struct page *lc_getPageNewData(struct fs *fs, uint64_t blocki, bool lock);
void lc_addPageBlockHash(struct gfs *gfs, struct fs *fs,
                         struct page *page, uint64_t block);
void lc_freeBlocksAfterFlush(struct fs *fs, uint64_t count);
void lc_flushPageCluster(struct gfs *gfs, struct fs *fs,
                         struct page *head, uint64_t count, bool bfree);
void lc_releasePages(struct gfs *gfs, struct fs *fs, struct page *head);
void lc_addPageForWriteBack(struct gfs *gfs, struct fs *fs, struct page *head,
                            struct page *tail, uint64_t pcount);
void *lc_cleaner(void *data);

uint64_t lc_copyPages(struct fs *fs, off_t off, size_t size,
                      struct dpage *dpages, struct fuse_bufvec *bufv,
                      struct fuse_bufvec *dst);
uint64_t lc_addPages(struct inode *inode, off_t off, size_t size,
                     struct dpage *dpages, uint64_t pcount);
void lc_readFile(fuse_req_t req, struct fs *fs, struct inode *inode,
                 off_t soffset, off_t endoffset, uint64_t asize,
                 struct page **pages, struct fuse_bufvec *bufv);
void lc_flushPages(struct gfs *gfs, struct fs *fs, struct inode *inode,
                   bool release, bool unlock);
void lc_truncateFile(struct inode *inode, off_t size, bool remove);
void lc_flushDirtyPages(struct gfs *gfs, struct fs *fs);
void lc_addDirtyInode(struct fs *fs, struct inode *inode);
void lc_flushDirtyInodeList(struct fs *fs, bool all);
void lc_invalidateDirtyPages(struct gfs *gfs, struct fs *fs);
void lc_wakeupCleaner(struct gfs *gfs, bool wait);
bool lc_flushInodeDirtyPages(struct inode *inode, uint64_t page, bool unlock,
                             bool force);
void lc_freePageData(struct gfs *gfs, struct fs *fs, char *data);
void lc_freePages(struct fs *fs, struct dpage *dpages, uint64_t pcount);

int lc_removeInode(struct fs *fs, struct inode *dir, ino_t ino, bool rmdir,
                   void **fsp);
void lc_epInit(struct fuse_entry_param *ep);

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
void lc_newLayer(fuse_req_t req, struct gfs *gfs, const char *name,
                 const char *parent, size_t size, bool rw);
void lc_removeLayer(fuse_req_t req, struct gfs *gfs, const char *name);
void lc_layerIoctl(fuse_req_t req, struct gfs *gfs, const char *name,
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
