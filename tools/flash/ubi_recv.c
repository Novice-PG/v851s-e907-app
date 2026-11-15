/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Abduwaris Nurmemet <daabu@qq.com>
 *
 * ubi_recv - stream a raw image from a serial tty into a UBI volume.
 *
 * Runs on the V851S (A7) from the initramfs shell, where the target volume is
 * not mounted.  The volume is opened for a UBI update, the ubiblock device (if
 * the kernel created one from the command line) is detached, and the payload
 * is read from the tty with XON/XOFF flow control so nothing is lost while
 * flash writes stall.
 *
 * Protocol (host side):
 *   1. host sends the magic "OPENUBI1" repeatedly until the board answers
 *   2. board prints "@@READY@@\n" and switches the tty to 1500000 baud
 *   3. host streams exactly <size> bytes
 *   4. board prints "\n@@GOT <n>@@\n"
 *
 * Usage: ubi_recv <vol_dev> <tty> <size>
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

#ifndef B1500000
#define B1500000 0010012
#endif

/* include/uapi/mtd/ubi-user.h */
#define UBI_IOCVOLUP    0x40084f00L
#define UBI_IOCVOLRMBLK 0x4f08

static const char MAGIC[] = "OPENUBI1";

static int write_all(int fd, const char *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

static int wait_magic(int fd)
{
	size_t mlen = sizeof(MAGIC) - 1;
	char win[sizeof(MAGIC)];
	size_t fill = 0;

	memset(win, 0, sizeof(win));
	while (1) {
		char c;

		if (read(fd, &c, 1) != 1) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (fill < mlen) {
			win[fill++] = c;
		} else {
			memmove(win, win + 1, mlen - 1);
			win[mlen - 1] = c;
		}
		if (fill == mlen && memcmp(win, MAGIC, mlen) == 0)
			return 0;
	}
}

int main(int argc, char **argv)
{
	const char *vol, *tty;
	long long size, got = 0;
	int vfd, tfd;
	struct termios old, raw, end;
	char buf[65536];

	if (argc < 4) {
		fprintf(stderr, "usage: %s vol_dev tty size\n", argv[0]);
		return 1;
	}
	vol = argv[1];
	tty = argv[2];
	size = atoll(argv[3]);
	if (size <= 0) {
		fprintf(stderr, "bad size\n");
		return 1;
	}

	vfd = open(vol, O_RDWR);
	if (vfd < 0) {
		perror("open volume");
		return 1;
	}
	if (ioctl(vfd, UBI_IOCVOLRMBLK, 0) == 0)
		fprintf(stderr, "ubiblock detached\n");
	if (ioctl(vfd, UBI_IOCVOLUP, &size) < 0) {
		perror("UBI_IOCVOLUP");
		return 1;
	}

	tfd = open(tty, O_RDWR | O_NOCTTY);
	if (tfd < 0) {
		perror("open tty");
		return 1;
	}
	if (tcgetattr(tfd, &old) < 0) {
		perror("tcgetattr");
		return 1;
	}
	raw = old;
	cfmakeraw(&raw);
	raw.c_iflag |= IXOFF;              /* ask the host to pause */
	raw.c_iflag &= ~(IXON | IXANY);    /* but pass 0x11/0x13 through */
	cfsetispeed(&raw, B1500000);
	cfsetospeed(&raw, B1500000);
	if (tcsetattr(tfd, TCSANOW, &raw) < 0) {
		perror("tcsetattr");
		return 1;
	}
	tcflush(tfd, TCIOFLUSH);

	fprintf(stderr, "waiting for host magic on %s\n", tty);
	if (wait_magic(tfd) < 0) {
		fprintf(stderr, "no magic\n");
		return 1;
	}
	write_all(tfd, "@@READY@@\n", 10);
	/* Drop any magic bytes the host sent while we were switching speed:
	 * they were received at the old baudrate and must not become payload. */
	tcdrain(tfd);
	tcflush(tfd, TCIFLUSH);

	while (got < size) {
		size_t want = sizeof(buf);
		ssize_t n;

		if ((long long)want > size - got)
			want = (size_t)(size - got);
		n = read(tfd, buf, want);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("read tty");
			break;
		}
		if (n == 0)
			break;
		if (write_all(vfd, buf, (size_t)n) < 0) {
			perror("write volume");
			break;
		}
		got += n;
		if ((got >> 22) != ((got - n) >> 22))
			write_all(tfd, ".", 1);
	}

	close(vfd);

	/* keep 1500000 baud but restore canonical mode for the shell */
	end = old;
	cfsetispeed(&end, B1500000);
	cfsetospeed(&end, B1500000);
	tcsetattr(tfd, TCSADRAIN, &end);

	{
		char msg[64];
		int l = snprintf(msg, sizeof(msg), "\n@@GOT %lld@@\n", got);

		write_all(tfd, msg, (size_t)l);
	}
	return got == size ? 0 : 2;
}
