#!/bin/sh
./autogen.sh
./configure
make gitrev
make -j
sudo make install
sudo ldconfig
sudo depmod
sudo modprobe zfs
dd if=/dev/zero of=/tmp/backing bs=1MiB count=256
sudo losetup -f /tmp/backing
sudo cmd/zstream/scripts/make-test-streams.sh /dev/loop0

