CC=gcc
CFLAGS=-g -Wall $ -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse
LDFLAGS=-ltcmalloc -pthread -lprofiler -lfuse
OBJ=lcfs.o memory.o fops.o super.o io.o extent.o block.o fs.o inode.o dir.o emap.o bcache.o page.o xattr.o clone.o stats.o

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
