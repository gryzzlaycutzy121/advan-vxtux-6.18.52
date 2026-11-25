/*
 * unisoc-tee-ipc - Linux userspace port of Unisoc/Android Trusty TIPC client
 * ---------------------------------------------------------------------------
 * Source of truth: stock ROM binaries pulled from the device super partition:
 *   /vendor/bin/tee_rpc      (Android ELF, bionic)
 *   /vendor/lib64/libtrusty.so (exports tipc_connect/tipc_send/tipc_close)
 *
 * Reverse-engineered ABI (verified against the 6.18 VXTux kernel port):
 *   drivers/trusty/trusty-ipc.c
 *     TIPC_IOC_MAGIC   = 'r'
 *     TIPC_IOC_CONNECT = _IOW('r', 0x80, char *)  -> 0x40087280
 *   device node        = /dev/trusty-ipc-dev0   (device_create "trusty-ipc-%s")
 *   service name       = e.g. "com.android.trusty.ion"
 *
 * The Android library talks to the TEE (Trusty secure world) through a
 * virtio-TIPC channel owned by the kernel trusty-ipc module. This userspace
 * tool speaks the exact same ioctl ABI, so it can drive the same channel on a
 * plain Linux userspace (no Android/bionic, no libnl, no logd).
 *
 * Build (aarch64 Linux, static):
 *   aarch64-none-linux-gnu-gcc -O2 -static -o unisoc-tee-ipc unisoc-tee-ipc.c
 *
 * Usage:
 *   unisoc-tee-ipc probe [service]
 *   unisoc-tee-ipc connect <service> [--hex payload]
 *   unisoc-tee-ipc send <service> <hex-bytes>
 *   unisoc-tee-ipc help
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* --- ABI from the 6.18 VXTux kernel (drivers/trusty/trusty-ipc.c) --------- */
#define TIPC_IOC_MAGIC		'r'
#define TIPC_IOC_CONNECT	_IOW(TIPC_IOC_MAGIC, 0x80, char *)

#define TIPC_DEV_DEFAULT	"/dev/trusty-ipc-dev0"
#define TIPC_DEFAULT_SERVICE	"com.android.trusty.ion"

#ifndef TIPC_DEV
#define TIPC_DEV TIPC_DEV_DEFAULT
#endif

/* The ioctl request number is architecture independent for _IOW with a
 * pointer argument, but assert it so a bad toolchain build fails loudly. */
#define TIPC_IOC_CONNECT_EXPECTED	0x40087280UL

static int tipc_open(const char *dev)
{
	int fd = open(dev, O_RDWR);
	if (fd < 0)
		return -1;
	return fd;
}

/*
 * tipc_connect: open the device and issue TIPC_IOC_CONNECT with the
 * NUL-terminated service name (mirrors libtrusty.so:tipc_connect).
 * Returns fd >= 0 on success, -errno on failure.
 */
static int tipc_connect(const char *dev, const char *service)
{
	int fd, ret;

	if (!service || !*service) {
		fprintf(stderr, "unisoc-tee-ipc: empty service name\n");
		return -EINVAL;
	}

	fd = tipc_open(dev);
	if (fd < 0) {
		fprintf(stderr, "unisoc-tee-ipc: cannot open %s: %s\n",
			dev, strerror(errno));
		return -errno;
	}

	ret = ioctl(fd, TIPC_IOC_CONNECT, (char *)service);
	if (ret < 0) {
		int e = errno;
		fprintf(stderr,
			"unisoc-tee-ipc: TIPC_IOC_CONNECT(0x%lx, \"%s\") failed: %s\n",
			(unsigned long)TIPC_IOC_CONNECT, service, strerror(e));
		close(fd);
		return -e;
	}

	return fd;
}

/* hex2bin: "aabbcc" -> bytes. Returns byte count or -1. */
static int hex2bin(const char *hex, unsigned char *out, size_t outmax)
{
	size_t n = 0;

	if (!hex)
		return -1;
	if (strlen(hex) % 2)
		return -1;

	while (*hex && n < outmax) {
		int hi, lo;
		char c = *hex++;

		hi = (c >= '0' && c <= '9') ? c - '0'
		   : (c >= 'a' && c <= 'f') ? c - 'a' + 10
		   : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
		if (hi < 0)
			return -1;
		c = *hex++;
		lo = (c >= '0' && c <= '9') ? c - '0'
		   : (c >= 'a' && c <= 'f') ? c - 'a' + 10
		   : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
		if (lo < 0)
			return -1;

		out[n++] = (unsigned char)((hi << 4) | lo);
	}
	return (int)n;
}

static void hexdump(const unsigned char *b, int len)
{
	int i;
	printf("recv %d bytes:", len);
	for (i = 0; i < len; i++) {
		if (i % 16 == 0)
			printf("\n  ");
		printf("%02x ", b[i]);
	}
	printf("\n");
}

static int cmd_probe(const char *service)
{
	int fd = tipc_connect(TIPC_DEV, service);
	if (fd < 0) {
		printf("probe: FAIL (errno %d: %s)\n", -fd, strerror(-fd));
		return 1;
	}
	printf("probe: OK - connected to service \"%s\" on %s\n", service, TIPC_DEV);
	close(fd);
	return 0;
}

static int cmd_send(const char *service, const char *hexpayload)
{
	unsigned char payload[4096];
	unsigned char rx[4096];
	int plen, fd, n;

	plen = hex2bin(hexpayload, payload, sizeof(payload));
	if (plen < 0) {
		fprintf(stderr, "unisoc-tee-ipc: payload must be even-length hex\n");
		return 2;
	}

	fd = tipc_connect(TIPC_DEV, service);
	if (fd < 0)
		return 1;

	if (write(fd, payload, (size_t)plen) != plen) {
		fprintf(stderr, "unisoc-tee-ipc: write failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	printf("sent %d bytes to \"%s\"\n", plen, service);

	n = (int)read(fd, rx, sizeof(rx));
	if (n < 0) {
		fprintf(stderr, "unisoc-tee-ipc: read failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	if (n == 0) {
		printf("recv: 0 bytes (channel closed / no reply)\n");
	} else {
		hexdump(rx, n);
	}

	close(fd);
	return 0;
}

static void usage(const char *argv0)
{
	printf(
"unisoc-tee-ipc - Unisoc Trusty TIPC client (Linux userspace)\n"
"\n"
"Usage:\n"
"  %s probe [service]\n"
"        Open the TIPC device and connect to <service>\n"
"        (default service: %s)\n"
"  %s send <service> <hex-bytes>\n"
"        Connect, write the hex payload, read one reply\n"
"  %s help\n"
"\n"
"Device : %s\n"
"Ioctl  : TIPC_IOC_CONNECT = 0x%lx\n",
		argv0, TIPC_DEFAULT_SERVICE, argv0, argv0,
		TIPC_DEV, (unsigned long)TIPC_IOC_CONNECT);
}

int main(int argc, char **argv)
{
	if (TIPC_IOC_CONNECT != TIPC_IOC_CONNECT_EXPECTED) {
		fprintf(stderr,
			"unisoc-tee-ipc: ABI mismatch: TIPC_IOC_CONNECT=0x%lx, expected 0x%lx\n",
			(unsigned long)TIPC_IOC_CONNECT,
			TIPC_IOC_CONNECT_EXPECTED);
		return 3;
	}

	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}

	if (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help") ||
	    !strcmp(argv[1], "-h")) {
		usage(argv[0]);
		return 0;
	}

	if (!strcmp(argv[1], "probe")) {
		const char *svc = (argc > 2) ? argv[2] : TIPC_DEFAULT_SERVICE;
		return cmd_probe(svc);
	}

	if (!strcmp(argv[1], "send")) {
		if (argc < 4) {
			fprintf(stderr,
				"unisoc-tee-ipc: send needs <service> <hex-bytes>\n");
			return 2;
		}
		return cmd_send(argv[2], argv[3]);
	}

	fprintf(stderr, "unisoc-tee-ipc: unknown command '%s'\n", argv[1]);
	usage(argv[0]);
	return 2;
}
