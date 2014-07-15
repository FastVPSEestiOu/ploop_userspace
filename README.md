ploop_userspace
===============

Tool for mounting [OpenVZ ploop](http://openvz.org/Ploop) images without support from kernel side.

Author: Pavel Odintsov pavel.odintsov at gmail.com from FastVPS.ru :)
License: GPLv2

How it works? We use [BUSE](https://github.com/acozzette/BUSE) and NBD for emulating block device without any support from kernel. We got some code from ploop project and reverse storgae format. But we can only read ploop images because writing is too dangerous and  complicated and will be result to incompatibility with in-kernel ploop.  

Installing:

You need only g++ and gcc for compiling this code.
```bash
cd /usr/src
git clone https://github.com/FastVPSEestiOu/ploop_userspace.git
make
```

Usage:
```bash
./ploop_userspace CTID
```

Mounting ploop file system to folder:
```bash
mount -r -o noload /dev/nbd0p1 /mnt
```

Examples:
```bash
We process: /vz/private/1204/root.hdd/root.hdd
Ploop file size is: 7247757312
version: 2 disk type: 2 heads count: 16 cylinder count: 65536 sector count: 2048 size in tracks: 16384 size in sectors: 33554432 disk in use: 1953459801 first block offset: 2048 flags: 0
We have 1 BAT blocks
We have 262128 slots in 0 map
Number of non zero blocks in map: 6911
We can store about 2951741440 bytes here
Set device /dev/nbd0 as read only
Try to found partitions on ploop device
You could mount ploop filesystem with command: mount -r -o noload /dev/nbd0p1 /mnt
```

FAQ:
* Can I got direct access to partition inside ploop? Yes!
* Can I got direct access to whole block device inside ploop? Yes!
* Can I mount non ext3/ext4 fs from ploop? yes!
* Can I send patches to you! YES YES YES!
* This code is stable? Not thoroughly.
* Could I mount plop for write? Not because it's too complicated
* What license of your code? GPLv2
* Yout code is fast enought? Yep! I can copy files from ploop in ~51MB/sec on SAS powered RAID-10.
