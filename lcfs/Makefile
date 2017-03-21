# lcfs Makefile -- 201701.06MeV
CC=gcc

ifndef BUILD_FLAGS
	override BUILD_FLAGS := $(BUILD_FLAGS) -g
endif

UNAME=$(shell uname)
ifeq ($(UNAME),Linux)
	CFLAGS=$(BUILD_FLAGS) -Wall -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse -I/usr/local/include/fuse
	LDFLAGS=-lz -ltcmalloc -pthread -lprofiler -lurcu -lfuse
	#CFLAGS=$(BUILD_FLAGS) -Wall -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse3 -I/usr/local/include/fuse3
	#LDFLAGS=-lz -ltcmalloc -pthread -lprofiler -L/usr/local/lib -lfuse3
else
	CFLAGS=$(BUILD_FLAGS) -Wno-format $ -D_FILE_OFFSET_BITS=64 -I/usr/local/include/osxfuse/fuse -I/usr/local/include/osxfuse
	LDFLAGS=-ltcmalloc -lprofiler -losxfuse -lz -lurcu
endif

ifdef STATIC
LCFSSTATICLIBS="libtcmalloc_and_profiler.a libfuse.a libunwind.a liburcu.a"
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

COBJ=cli.o daemon.o ioctl.o memory.o fops.o super.o io.o extent.o block.o fs.o inode.o dir.o emap.o bcache.o page.o xattr.o layer.o hlink.o diff.o stats.o debug.o
ifeq ($(UNAME),Linux)
OBJ=$(COBJ) linux.o
else
OBJ=$(COBJ) apple.o
endif

all: lcfs

lcfs: version $(OBJ)
	$(CC) $(OBJ) -o $@ $(CFLAGS) $(LDFLAGS)

version:
	@(mkdir -p version && cd version && ../version_gen.sh)

clean:
	rm -fr *.o lcfs testxattr testdiff

testxattr: testxattr.o
	$(CC) $^ -o $@ $(CFLAGS) $(LDFLAGS)

testdiff: testdiff.o
	$(CC) $^ -o $@ $(CFLAGS) $(LDFLAGS)

test: lcfs testxattr testdiff
	sudo ./test.sh

rpm:
	@cd rpm && BUILD_FLAGS="${BUILD_FLAGS}" VERSION="`VERSION="$(VERSION)" ../version_gen.sh -p`" REVISION="$(REVISION)" ./buildrpm.sh

.PHONY: rpm version clean
