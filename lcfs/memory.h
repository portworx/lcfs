#ifndef _MEMORY_H
#define _MEMORY_H

/* Type of malloc requests */
enum lc_memTypes {
    LC_MEMTYPE_GFS = 0,             /* Global allocations */
    LC_MEMTYPE_DIRENT = 1,          /* Directory entries */
    LC_MEMTYPE_DCACHE = 2,          /* Directory hash table */
    LC_MEMTYPE_ICACHE = 3,          /* Inode cache hash table */
    LC_MEMTYPE_INODE = 4,           /* Inodes */
    LC_MEMTYPE_LBCACHE = 5,         /* Base layer block cache hash table */
    LC_MEMTYPE_PCACHE = 6,          /* Inode dirty page list */
    LC_MEMTYPE_PCLOCK = 7,          /* Locks for block cache hash lists */
    LC_MEMTYPE_EXTENT = 8,          /* Emap/Space extents */
    LC_MEMTYPE_BLOCK = 9,           /* Metadata blocks */
    LC_MEMTYPE_PAGE = 10,           /* Page headers */
    LC_MEMTYPE_DATA = 11,           /* Data blocks */
    LC_MEMTYPE_DPAGEHASH = 12,      /* Dirty page hash table */
    LC_MEMTYPE_HPAGE = 13,          /* Dirty pages */
    LC_MEMTYPE_XATTR = 14,          /* Extended attributes */
    LC_MEMTYPE_XATTRNAME = 15,      /* Extended attribute names */
    LC_MEMTYPE_XATTRVALUE = 16,     /* Extended attribute values */
    LC_MEMTYPE_XATTRBUF = 17,       /* Extended attributes buffers */
    LC_MEMTYPE_XATTRINODE = 18,     /* Extended attribute portion in inode */
    LC_MEMTYPE_CFILE = 19,          /* Tracking a file change */
    LC_MEMTYPE_CDIR = 20,           /* Tracking a directory change */
    LC_MEMTYPE_PATH = 21,           /* Path to a directory */
    LC_MEMTYPE_HLDATA = 22,         /* Hard links */
    LC_MEMTYPE_STATS = 23,          /* Request stats */
    LC_MEMTYPE_MAX = 24,
};

#endif
