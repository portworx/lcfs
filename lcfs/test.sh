#!/bin/bash -x

MNT=/tmp/lcfs-testmountpoint
MNT2=/lcfs
LCFS=$PWD/lcfs
XATTR=$PWD/testxattr
TESTDIFF=$PWD/testdiff
CSTAT=$PWD/cstat

umount -f $MNT $MNT2 2>/dev/null
sleep 10
rm -fr $MNT $MNT2 2>/dev/null
mkdir $MNT $MNT2

DEVICE=/tmp/lcfs-testdevice
rm $DEVICE 2>/dev/null
dd if=/dev/zero of=$DEVICE count=120000 bs=4096

$LCFS $DEVICE $MNT $MNT2
sleep 10
cd $MNT

df -k $MNT
df -i $MNT
ls
ls -l
ls -l file
ls -l dir

touch file
ls file
ls -Ril file
cat file
chmod 777 file
chown nobody file
chgrp nogroup file
touch file

mkdir dir
ls -Ril dir
rmdir dir

ln -s file file1
ls file1
cat file1
rm file1

mknod block b 10 5
mknod char c 10 5
mknod unbuffer u 10 5
mknod fifo p
ls -lRi

ln file file1
ln fifo fifo.lnk
ls -Ril
cat file1
rm file1
mkdir dir
ln file dir/file

mv file file1
ls -liR
touch file2
mv file1 file2
mv file2 dir
ls -liR
touch file
mv file dir/file1
ls -liR
touch file1
mv file1 dir/file1
ls -liR

mkdir -p rdir rdir1/rdir
mv rdir1/rdir .

mkdir -p rdir2 rdir1/rdir2
touch rdir2/file
mv rdir1/rdir2 .

rm -fr $MNT/*

mkdir -p dir/dir1/dir2
touch dir/file
touch dir/dir1/file
touch dir/dir1/dir2/file
rmdir dir

cp /etc/passwd .
cat passwd > /dev/null

dd if=/dev/zero of=file count=1000 bs=4096
cat file > /dev/null

dd if=/dev/urandom of=file count=10 bs=4096
ls -l file
dd if=/dev/urandom of=file count=10 bs=4096 conv=notrunc
ls -l file
dd if=/dev/urandom of=file count=10 bs=4096 seek=5 conv=notrunc
ls -l file
dd if=/dev/urandom of=file count=10 bs=4096 seek=5
dd if=/dev/urandom of=file count=10 bs=4096 seek=2 conv=fdatasync
dd if=/dev/urandom of=file count=10 bs=4096 seek=10 conv=fsync
ls -l file

dd if=/dev/urandom of=file1 count=1 bs=1024 seek=23 conv=notrunc
dd if=/dev/urandom of=file1 count=1 bs=1024 seek=23 conv=notrunc
dd if=/dev/urandom of=file1 count=1 bs=1024 seek=22 conv=notrunc
dd if=/dev/urandom of=file1 count=1 bs=1024 seek=24 conv=notrunc

$XATTR

rm -fr file file1 passwd

ls -ltRi

echo hello > file
echo foo > file
cat /etc/passwd > file
cat /etc/passwd >> file
rm file

cd -

service docker stop
dockerd -s vfs -g $MNT >/dev/null &
sleep 3
docker plugin install --grant-all-permissions portworx/lcfs
docker plugin ls
pkill dockerd
sleep 3
sudo dockerd --experimental -s portworx/lcfs -g $MNT >/dev/null &
sleep 10
docker run hello-world

cd $MNT/lcfs
$CSTAT .
for layer in *
do
    $TESTDIFF $layer
done
cd -

docker ps --all --format {{.ID}} | xargs docker rm
docker rmi hello-world
pkill dockerd
sleep 10

rmdir $MNT/lcfs
mkdir $MNT/lcfs/dir
touch $MNT/lcfs/file

df -k $MNT
df -i $MNT

#Create a fragmented file spanning multiple emap blocks.
#Create a directory spanning multiple blocks.
cd $MNT
mkdir dir
set +x
for (( i = 0; i < 500; i += 2 ))
do
    dd if=/dev/urandom of=file conv=notrunc count=1 bs=4096 seek=$i 2&>/dev/null
    touch dir/file$i
done
set -x
cd -

umount -f $MNT/plugins/*/rootfs/lcfs
umount -f $MNT $MNT2 2>/dev/null
sleep 10

$LCFS $DEVICE $MNT $MNT2
sleep 10
cd $MNT
ls -ltRi > /dev/null
stat file
stat dir

set +x
for (( i = 0; i < 500; i += 2 ))
do
    dd if=/dev/urandom of=file conv=notrunc count=1 bs=4096 seek=$i 2&>/dev/null
    rm dir/file$i
done
set -x
rmdir dir
cd -

umount -f $MNT $MNT2 2>/dev/null
sleep 10

$LCFS $DEVICE $MNT $MNT2
sleep 10
cd $MNT

ls -ltRi > /dev/null
touch file
dd if=/dev/urandom of=file count=10 bs=4096
rm -fr $MNT/*
cd -

df -k $MNT
df -i $MNT

umount -f $MNT $MNT2 2>/dev/null
sleep 10
rm -fr $MNT $MNT2 $DEVICE
wait
