CC=gcc
CFLAGS=-Wall $ `pkg-config fuse --cflags --libs`
OBJ=dfs.o fops.o super.o io.o fs.o inode.o dir.o page.o clone.o
dfs: $(OBJ)
	$(CC) $^ -o $@ $(CFLAGS)

clean:
	rm *.o dfs
