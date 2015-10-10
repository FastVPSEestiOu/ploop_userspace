ploop_userspace
===============

Tool for mounting [OpenVZ ploop](http://openvz.org/Ploop) images without support from kernel side.

Author: Pavel Odintsov pavel.odintsov at gmail.com from FastVPS.ru :)
License: GPLv2

How it works? We use [BUSE](https://github.com/acozzette/BUSE) and NBD for emulating block device without any support from kernel. We got some code from ploop project and reverse storage format. We can only read ploop images because writing to a ploop file with this tool is too dangerous yet and complicated and will be result in incompatibility with in-kernel ploop.

Compatibility:
* ploop v1 read only support - full
* ploop v2 read only support - full

Installing:

Requirements for Debian:
```bash
apt-get install -y kpartx build-essential git parted
```

Requirements for CentOS:
```bash
yum install -y git gcc gcc-c++ make
```

You need only g++ and gcc for compiling this code.
```bash
cd /usr/src
git clone https://github.com/FastVPSEestiOu/ploop_userspace.git
cd ploop_userspace
make
```

Usage:
```bash
./ploop_userspace CTID
or
./ploop_userspace /vz/private/7777/root.hdd/root.hdd
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

For debugging purposes you can add flag for dump all I/O operations:
```bash
TRACE_REQUESTS=1 ./ploop_userspace 1202
```

Remarks about I/O scheduler for NBD (you should aware of cfq scheduler because it can broke nbd):
* https://onapp.zendesk.com/entries/42013023-NBD-Scheduler-set-incorrectly
* https://github.com/yoe/nbd
* https://www.mail-archive.com/git-commits-head@vger.kernel.org/msg41533.html


FAQ:
* Can I get direct access to partitions inside ploop? Yes!
* Can I get direct access to the whole block device inside ploop? Yes!
* Can I mount non ext3/ext4 filesystems from ploop? Yes!
* Can I send patches to you? YES YES YES!
* Is this code stable yet? Not thoroughly.
* Can I somehow mount the ploop device writable with this tool? No, because implementing the writing technology is too complicated and may result in incompatibility with the real ploop technology from OpenVZ.
* What license is your code? GPLv2
* Isn't a userspace mount slow as hell? Nope, I can copy files from the ploop device with it at about ~51MB/s on a SAS powered RAID 10 system.
