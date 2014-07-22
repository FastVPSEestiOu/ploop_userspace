/*
 * buse - block-device userspace extensions
 * Copyright (C) 2013 Adam Cozzette
 *
 * This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/types.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "buse.h"

/*
 * These helper functions were taken from cliserv.h in the nbd distribution.
 */
#ifdef WORDS_BIGENDIAN
u_int64_t ntohll(u_int64_t a) {
  return a;
}
#else
u_int64_t ntohll(u_int64_t a) {
  u_int32_t lo = a & 0xffffffff;
  u_int32_t hi = a >> 32U;
  lo = ntohl(lo);
  hi = ntohl(hi);
  return ((u_int64_t) lo) << 32U | hi;
}
#endif
#define htonll ntohll

static int read_all(int fd, char* buf, size_t count)
{
  int bytes_read;

  while (count > 0) {
    bytes_read = read(fd, buf, count);
    assert(bytes_read > 0);
    buf += bytes_read;
    count -= bytes_read;
  }
  assert(count == 0);

  return 0;
}

static int write_all(int fd, char* buf, size_t count)
{
  int bytes_written;

  while (count > 0) {
    bytes_written = write(fd, buf, count);
    assert(bytes_written > 0);
    buf += bytes_written;
    count -= bytes_written;
  }
  assert(count == 0);

  return 0;
}

int buse_main(const char* dev_file, const struct buse_operations *aop, void *userdata)
{
  int sp[2];
  int nbd, sk, err, tmp_fd;
  u_int64_t from;
  u_int32_t len;
  ssize_t bytes_read;
  struct nbd_request request;
  struct nbd_reply reply;
  void *chunk;

  assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sp));

  nbd = open(dev_file, O_RDWR);
  assert(nbd != -1);

  assert(ioctl(nbd, NBD_SET_SIZE, aop->size) != -1);
  assert(ioctl(nbd, NBD_CLEAR_SOCK) != -1);

  // patch oditnsov
  // #define BLKROSET   _IO(0x12,93) /* set device read-only (0 = read-write) */ 
  // assert(ioctl(nbd, BLKROSET, 1)<0);

  if (!fork()) {
    /* The child needs to continue setting things up. */
    close(sp[0]);
    sk = sp[1];

    if(ioctl(nbd, NBD_SET_SOCK, sk) == -1){
      fprintf(stderr, "ioctl(nbd, NBD_SET_SOCK, sk) failed.[%s]\n", strerror(errno));
    }
#if defined NBD_SET_FLAGS && defined NBD_FLAG_SEND_TRIM
    else if(ioctl(nbd, NBD_SET_FLAGS, NBD_FLAG_SEND_TRIM) == -1){
      fprintf(stderr, "ioctl(nbd, NBD_SET_FLAGS, NBD_FLAG_SEND_TRIM) failed.[%s]\n", strerror(errno));
    }
#endif
    else{
      err = ioctl(nbd, NBD_DO_IT);
      fprintf(stderr, "nbd device terminated with  with err code: %d err text: %s\n", errno, strerror(errno));
      if (err == -1) {
	fprintf(stderr, "%s\n", strerror(errno));
      }
    }

    ioctl(nbd, NBD_CLEAR_QUE);
    ioctl(nbd, NBD_CLEAR_SOCK);

    exit(0);
  }

  /* The parent opens the device file at least once, to make sure the
   * partition table is updated. Then it closes it and starts serving up
   * requests. */

  tmp_fd = open(dev_file, O_RDONLY);
  assert(tmp_fd != -1);
  close(tmp_fd);

  close(sp[1]);
  sk = sp[0];

  reply.magic = htonl(NBD_REPLY_MAGIC);
  reply.error = htonl(0);

  while ((bytes_read = read(sk, &request, sizeof(request))) > 0) {
    assert(bytes_read == sizeof(request));
    memcpy(reply.handle, request.handle, sizeof(reply.handle));

    len = ntohl(request.len);
    from = ntohll(request.from);
    assert(request.magic == htonl(NBD_REQUEST_MAGIC));

    switch(ntohl(request.type)) {
      /* I may at some point need to deal with the the fact that the
       * official nbd server has a maximum buffer size, and divides up
       * oversized requests into multiple pieces. This applies to reads
       * and writes.
       */
    case NBD_CMD_READ:
      // commented By Odintsov
      //fprintf(stderr, "Request for read of size %d\n", len);
      assert(aop->read);
      chunk = malloc(len);
      reply.error = aop->read(chunk, len, from, userdata);
      write_all(sk, (char*)&reply, sizeof(struct nbd_reply));
      if(reply.error == 0)
	write_all(sk, (char*)chunk, len);
      free(chunk);
      break;
    case NBD_CMD_WRITE:
      // commented By Odintsov
      // fprintf(stderr, "Request for write of size %d\n", len);
      assert(aop->write);
      chunk = malloc(len);
      read_all(sk, chunk, len);
      reply.error = aop->write(chunk, len, from, userdata);
      free(chunk);
      write_all(sk, (char*)&reply, sizeof(struct nbd_reply));
      break;
    case NBD_CMD_DISC:
      /* Handle a disconnect request. */
      assert(aop->disc);
      aop->disc(userdata);
      return 0;
#ifdef NBD_FLAG_SEND_FLUSH
    case NBD_CMD_FLUSH:
      assert(aop->flush);
      reply.error = aop->flush(userdata);
      write_all(sk, (char*)&reply, sizeof(struct nbd_reply));
      break;
#endif
#ifdef NBD_FLAG_SEND_TRIM
    case NBD_CMD_TRIM:
      assert(aop->trim);
      reply.error = aop->trim(from, len, userdata);
      write_all(sk, (char*)&reply, sizeof(struct nbd_reply));
      break;
#endif
    default:
      assert(0);
    }
  }
  if (bytes_read == -1)
    fprintf(stderr, "%s\n", strerror(errno));
  return 0;
}
