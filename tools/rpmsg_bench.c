/*
 * rpmsg_bench - round-trip benchmark for the E907 rpmsg echo endpoint.
 *
 * Usage: rpmsg_bench [count] [size]
 *   defaults: 1000 messages of 32 bytes
 *
 * Opens /dev/rpmsg_ctrl0, creates the "rpmsg_chrdev" endpoint, then sends
 * `count` messages back to back and waits for each echo, reporting min/avg/max
 * round-trip time and any errors.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>

struct rpmsg_endpoint_info {
	char name[32];
	uint32_t src;
	uint32_t dst;
};

#define RPMSG_CREATE_EPT_IOCTL  _IOW(0xb5, 0x1, struct rpmsg_endpoint_info)
#define RPMSG_ADDR_ANY          0xFFFFFFFFu

static int dev_exists(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

int main(int argc, char **argv)
{
	const char *ctrl = "/dev/rpmsg_ctrl0";
	const char *name = "rpmsg_chrdev";
	int count = (argc > 1) ? atoi(argv[1]) : 1000;
	int size = (argc > 2) ? atoi(argv[2]) : 32;
	char path[64], buf[4096];
	int before[64], fd, ept_fd, i, ret, n;
	struct rpmsg_endpoint_info info;
	uint64_t t0, t1, sum = 0, worst = 0, best = ~0ull;
	int ok = 0, bad = 0;

	if (size <= 0 || size > (int)sizeof(buf))
		size = 32;

	for (i = 0; i < 64; i++) {
		snprintf(path, sizeof(path), "/dev/rpmsg%d", i);
		before[i] = dev_exists(path);
	}

	fd = open(ctrl, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", ctrl, strerror(errno));
		return 1;
	}

	memset(&info, 0, sizeof(info));
	strncpy(info.name, name, sizeof(info.name) - 1);
	info.src = RPMSG_ADDR_ANY;
	info.dst = 0;

	ret = ioctl(fd, RPMSG_CREATE_EPT_IOCTL, &info);
	if (ret < 0) {
		fprintf(stderr, "RPMSG_CREATE_EPT_IOCTL(%s): %s\n",
			name, strerror(errno));
		close(fd);
		return 1;
	}

	ept_fd = -2;
	for (n = 0; n < 50 && ept_fd == -2; n++) {
		usleep(100000);
		for (i = 0; i < 64; i++) {
			snprintf(path, sizeof(path), "/dev/rpmsg%d", i);
			if (!before[i] && dev_exists(path)) {
				ept_fd = open(path, O_RDWR);
				if (ept_fd >= 0)
					break;
			}
		}
	}
	if (ept_fd < 0) {
		fprintf(stderr, "no new /dev/rpmsgN appeared\n");
		close(fd);
		return 1;
	}

	memset(buf, 'A', size);
	for (i = 0; i < count; i++) {
		fd_set rfds;
		struct timeval tv;

		t0 = now_ns();
		ret = write(ept_fd, buf, size);
		if (ret != size) {
			fprintf(stderr, "write %d: %s\n", i, strerror(errno));
			bad++;
			continue;
		}

		FD_ZERO(&rfds);
		FD_SET(ept_fd, &rfds);
		tv.tv_sec = 3;
		tv.tv_usec = 0;
		ret = select(ept_fd + 1, &rfds, NULL, NULL, &tv);
		if (ret <= 0) {
			fprintf(stderr, "msg %d: no echo within 3s\n", i);
			bad++;
			continue;
		}

		ret = read(ept_fd, buf, sizeof(buf));
		t1 = now_ns();
		if (ret != size) {
			fprintf(stderr, "msg %d: short echo (%d)\n", i, ret);
			bad++;
			continue;
		}
		ok++;

		t1 -= t0;
		sum += t1;
		if (t1 < best)
			best = t1;
		if (t1 > worst)
			worst = t1;
	}

	printf("count=%d size=%d ok=%d bad=%d\n", count, size, ok, bad);
	if (ok)
		printf("rtt min=%llu us avg=%llu us max=%llu us\n",
		       (unsigned long long)(best / 1000),
		       (unsigned long long)(sum / ok / 1000),
		       (unsigned long long)(worst / 1000));

	close(ept_fd);
	close(fd);
	return bad ? 2 : 0;
}
