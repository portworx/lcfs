# lcfs Makefile -- 201701.06MeV
CC=gcc
CFLAGS=-g -Wall -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse -I/usr/local/include/fuse
LDFLAGS=-ltcmalloc -pthread -lprofiler -lfuse

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

LDFLAGS=-pthread $(LCFS_STATIC_LIBS) -lstdc++ -lm -ldl $(LCFS_LZMA_LIBS)
endif  # STATIC

#CFLAGS=-g -Wall $ -D_FILE_OFFSET_BITS=64 -I/usr/local/include/fuse3
#LDFLAGS=-ltcmalloc -pthread -L/usr/local/lib -lfuse3
OBJ=lcfs.o memory.o fops.o super.o io.o extent.o block.o fs.o inode.o dir.o emap.o bcache.o page.o xattr.o layer.o stats.o

all: lcfs cstat

lcfs: $(OBJ)
	$(CC) $^ -o $@ $(CFLAGS) $(LDFLAGS)

cstat: cstat.o
	$(CC) $^ -o $@ $(CFLAGS) $(LDFLAGS)

clean:
	rm -fr *.o lcfs cstat testxattr

testxattr: testxattr.o
	$(CC) $^ -o $@ $(CFLAGS) $(LDFLAGS)

test: lcfs testxattr
	sudo ./test.sh

rpm:
	@cd rpm && ./buildrpm.sh

.PHONY: rpm
