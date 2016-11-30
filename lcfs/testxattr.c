#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/xattr.h>
#include <assert.h>

#define TESTFILE  ".xattr.test"

/* Test various extended attributes operations */
int
main() {
    char buf[32];
    size_t size;
    int err;

    rmdir(TESTFILE);
    unlink(TESTFILE);
    err = mkdir(TESTFILE, 0777);
    assert(err == 0);
    size = getxattr(TESTFILE, "attr", NULL, 0);
    assert(size == -1);
    err = setxattr(TESTFILE, "attr", NULL, 0, 0);
    assert(err == 0);
    err = setxattr(TESTFILE, "attr1", "value", 5, 0);
    assert(err == 0);
    err = setxattr(TESTFILE, "attr1", "value1", 6, 0);
    assert(err == 0);
    err = setxattr(TESTFILE, "attr1", "val", 3, 0);
    assert(err == 0);
    err = setxattr(TESTFILE, "attr1", NULL, 0, 0);
    assert(err == 0);
    err = setxattr(TESTFILE, "attr1", NULL, 0, XATTR_CREATE);
    assert(err != 0);
    err = setxattr(TESTFILE, "attr2", "value", 5, XATTR_CREATE);
    assert(err == 0);
    err = setxattr(TESTFILE, "attr2", "value2", 6, XATTR_REPLACE);
    assert(err == 0);
    err = setxattr(TESTFILE, "attr3", "val", 3, 0);
    assert(err == 0);
    err = setxattr(TESTFILE, "attr4", "value2", 6, XATTR_REPLACE);
    assert(err != 0);
    size = getxattr(TESTFILE, "attr3", NULL, 0);
    assert(size == 3);
    size = getxattr(TESTFILE, "attr3", buf, size);
    assert(size == 3);
    size = listxattr(TESTFILE, NULL, 0);
    assert(size == 23);
    size = listxattr(TESTFILE, buf, size);
    assert(size == 23);
    err = removexattr(TESTFILE, "attr");
    assert(err == 0);
    err = removexattr(TESTFILE, "attr1");
    assert(err == 0);
    err = removexattr(TESTFILE, "attr2");
    assert(err == 0);
    err = removexattr(TESTFILE, "attr3");
    assert(err == 0);
    err = removexattr(TESTFILE, "attr3");
    assert(err != 0);
    size = listxattr(TESTFILE, NULL, 0);
    assert(size == 0);
    size = getxattr(TESTFILE, "attr3", NULL, 0);
    assert(size == -1);
    err = setxattr(TESTFILE, "attr3", "val", 3, 0);
    assert(err == 0);
    rmdir(TESTFILE);
    return 0;
}
