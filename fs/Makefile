CC=gcc
CFLAGS=-Wall $ `pkg-config fuse --cflags --libs`
OBJ=lcfs.o fops.o super.o io.o block.o fs.o inode.o dir.o bmap.o page.o xattr.o clone.o stats.o
lcfs: $(OBJ)
	$(CC) $^ -o $@ $(CFLAGS) -ltcmalloc

cstat: cstat.o
	$(CC) $^ -o $@ $(CFLAGS) -ltcmalloc

clean:
	rm *.o lcfs

test:
	sudo ./test.sh
