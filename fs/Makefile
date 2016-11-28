CC=gcc
CFLAGS=-O3 -Wall $ `pkg-config fuse --cflags --libs`
OBJ=lcfs.o fops.o super.o io.o block.o fs.o inode.o dir.o bmap.o page.o xattr.o clone.o stats.o

all: lcfs cstat

lcfs: $(OBJ)
	$(CC) $^ -o $@ $(CFLAGS) -ltcmalloc

cstat: cstat.o
	$(CC) $^ -o $@ $(CFLAGS) -ltcmalloc

clean:
	rm -fr *.o lcfs cstat testxattr

testxattr: testxattr.o
	$(CC) $^ -o $@ $(CFLAGS) -ltcmalloc

test: lcfs testxattr
	sudo ./test.sh
