CC=gcc
CFLAGS=-Wall $ `pkg-config fuse --cflags --libs`
OBJ=dfs.o fops.o super.o io.o block.o fs.o inode.o dir.o page.o xattr.o clone.o stats.o
dfs: $(OBJ)
	$(CC) $^ -o $@ $(CFLAGS) -ltcmalloc

clean:
	rm *.o dfs

test:
	sudo ./test.sh
