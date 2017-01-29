# lcfs Makefile -- 201701.06MeV
CC=gcc
UNAME=$(shell uname)
ifeq ($(UNAME),Linux)
	CFLAGS=-g -Wall -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse -I/usr/local/include/fuse
	LDFLAGS=-lz -ltcmalloc -pthread -lprofiler -lfuse
	#CFLAGS=-g -Wall -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse3 -I/usr/local/include/fuse3
	#LDFLAGS=-lz -ltcmalloc -pthread -lprofiler -L/usr/local/lib -lfuse3
else
	CFLAGS=-g -Wno-format $ -D_FILE_OFFSET_BITS=64 -I/usr/local/include/osxfuse/fuse -I/usr/local/include/osxfuse
	LDFLAGS=-ltcmalloc -lprofiler -losxfuse -lz
endif

ifdef STATIC
LCFSSTATICLIBS="libtcmalloc_and_profiler.a libfuse.a libunwind.a"
# use for loop to preserve library order
LCFS_STATIC_LIBS=$(shell for fl in "$(LCFSSTATICLIBS)"; do find /usr -name $$fl 2>/dev/null; done)
ifeq ($(shell test -z "$(LCFS_STATIC_LIBS)"; echo $$?),0)
$(error Unable to find static libraries to build lcfs.)
endif   # test LCFS_STATIC_LIBS

CHECK_LZMA_LIBS=$(shell find /usr -name libunwind.so 2>/dev/null)
ifeq ($(shell test -n "$(CHECK_LZMA_LIBS)"; echo $$?),0)
LCFS_LZMA_LIBS=$(shell ldd $(CHECK_LZMA_LIBS) | grep -q lzma && find /usr -name liblzma.a)
ifeq ($(shell test -z "$(LCFS_LZMA_LIBS)"; echo $$?),0)
LCFS_LZMA_LIBS=-llzma
endif     # test LCFS_LZMA_LIBS
endif  # test CHECK_LZMA_LIBS

LDFLAGS=-lz -pthread $(LCFS_STATIC_LIBS) -lstdc++ -lm -ldl $(LCFS_LZMA_LIBS)
endif  # STATIC

ifeq ($(UNAME),Linux)
OBJ=lcfs.o memory.o fops.o super.o io.o extent.o block.o fs.o inode.o dir.o emap.o bcache.o page.o xattr.o layer.o stats.o linux.o
else
OBJ=lcfs.o memory.o fops.o super.o io.o extent.o block.o fs.o inode.o dir.o emap.o bcache.o page.o xattr.o layer.o stats.o apple.o
endif

all: lcfs cstat

lcfs: version $(OBJ)
	$(CC) $(OBJ) -o $@ $(CFLAGS) $(LDFLAGS)

cstat: version cstat.o
	$(CC) cstat.o -o $@ $(CFLAGS) $(LDFLAGS)

version:
	@(mkdir -p version && cd version && ../version_gen.sh)

clean:
	rm -fr *.o lcfs cstat testxattr

testxattr: testxattr.o
	$(CC) $^ -o $@ $(CFLAGS) $(LDFLAGS)

test: lcfs testxattr
	sudo ./test.sh

rpm:
	@cd rpm && VERSION="`VERSION="$(VERSION)" ../version_gen.sh -p`" REVISION=$(REVISION) ./buildrpm.sh

.PHONY: rpm version clean
