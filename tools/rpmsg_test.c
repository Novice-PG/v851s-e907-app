/*
 * rpmsg_test - create an rpmsg endpoint on the A7 and talk to the E907.
 *
 * Usage: rpmsg_test [ctrl_dev] [ept_name] [message]
 *   defaults: /dev/rpmsg_ctrl0  rpmsg_chrdev  "hello e907"
 *
 * Opens the rpmsg control device, asks the kernel for a new endpoint
 * (RPMSG_CREATE_EPT_IOCTL), then opens the resulting /dev/rpmsgN, sends
 * one message and waits for the echo coming back from the E907 firmware.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>

struct rpmsg_endpoint_info {
	char name[32];
	uint32_t src;
	uint32_t dst;
};

#define RPMSG_CREATE_EPT_IOCTL  _IOW(0xb5, 0x1, struct rpmsg_endpoint_info)
#define RPMSG_DESTROY_EPT_IOCTL _IO(0xb5, 0x2)
#define RPMSG_ADDR_ANY          0xFFFFFFFFu

static int dev_exists(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0;
}

int main(int argc, char **argv)
{
	const char *ctrl = (argc > 1) ? argv[1] : "/dev/rpmsg_ctrl0";
	const char *name = (argc > 2) ? argv[2] : "rpmsg_chrdev";
	const char *text = (argc > 3) ? argv[3] : "hello e907";
	char path[64], buf[256];
	int before[64], fd, ept_fd, i, ret, n;
	struct rpmsg_endpoint_info info;

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
	info.src = RPMSG_ADDR_ANY;	/* let the kernel pick a local address */
	info.dst = 0;			/* channel destination is already set */

	ret = ioctl(fd, RPMSG_CREATE_EPT_IOCTL, &info);
	if (ret < 0) {
		fprintf(stderr, "RPMSG_CREATE_EPT_IOCTL(%s): %s\n",
			name, strerror(errno));
		close(fd);
		return 1;
	}
	printf("created endpoint %s\n", name);

	/* the endpoint device shows up through devtmpfs; poll briefly */
	ept_fd = -2;
	for (n = 0; n < 50 && ept_fd == -2; n++) {
		usleep(100000);
		for (i = 0; i < 64; i++) {
			snprintf(path, sizeof(path), "/dev/rpmsg%d", i);
			if (!before[i] && dev_exists(path)) {
				ept_fd = open(path, O_RDWR);
				if (ept_fd >= 0) {
					printf("endpoint device: %s\n", path);
					break;
				}
			}
		}
	}
	if (ept_fd < 0) {
		fprintf(stderr, "no new /dev/rpmsgN appeared\n");
		close(fd);
		return 1;
	}

	ret = write(ept_fd, text, strlen(text) + 1);
	if (ret < 0) {
		fprintf(stderr, "write: %s\n", strerror(errno));
		close(ept_fd);
		close(fd);
		return 1;
	}
	printf("sent %d bytes: %s\n", ret, text);

	fd_set rfds;
	struct timeval tv;
	FD_ZERO(&rfds);
	FD_SET(ept_fd, &rfds);
	tv.tv_sec = 3;
	tv.tv_usec = 0;
	ret = select(ept_fd + 1, &rfds, NULL, NULL, &tv);
	if (ret <= 0) {
		fprintf(stderr, "no echo within 3s (%s)\n",
			ret == 0 ? "timeout" : strerror(errno));
		close(ept_fd);
		close(fd);
		return 2;
	}

	memset(buf, 0, sizeof(buf));
	ret = read(ept_fd, buf, sizeof(buf) - 1);
	if (ret < 0) {
		fprintf(stderr, "read: %s\n", strerror(errno));
		close(ept_fd);
		close(fd);
		return 1;
	}
	printf("echo %d bytes: %s\n", ret, buf);
	printf("RESULT: %s\n",
	       (size_t)ret >= strlen(text) && !strncmp(buf, text, strlen(text))
	       ? "OK" : "MISMATCH");

	close(ept_fd);
	close(fd);
	return 0;
}
