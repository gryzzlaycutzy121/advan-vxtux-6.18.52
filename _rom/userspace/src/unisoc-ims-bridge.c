/*
 * unisoc-ims-bridge - Linux userspace port of Unisoc/Android IMS bridge
 * ---------------------------------------------------------------------------
 * Source of truth: stock ROM binaries pulled from the device super partition:
 *   /system_ext/bin/ims_bridged         (Android ELF, bionic, uses libnl)
 *   /system_ext/lib64/libimsbrd.so      (14 exported imsbrd_* helpers)
 *
 * Reverse-engineered ABI (verified against the 6.18 VXTux kernel port):
 *   net/ims_bridge/imsbr_netlink.c
 *     generic netlink family name = "imsbr"   (IMSBR_GENL_NAME)
 *     family version           = 1           (IMSBR_GENL_VERSION)
 *     commands (enum imsbr_commands, uapi/linux/ims_bridge/ims_bridge.h):
 *       1 IMSBR_C_CALL_STATE     2 IMSBR_C_ADD_TUPLE
 *       3 IMSBR_C_DEL_TUPLE      4 IMSBR_C_RESET_TUPLE
 *       5 IMSBR_C_SEND_LOCALMAC  6 IMSBR_C_SEND_REMOTEMAC
 *       7 IMSBR_C_LOWPOWER_ST    8 IMSBR_C_ADD_SPI
 *       9 IMSBR_C_DEL_SPI
 *     attributes (enum imsbr_attrs):
 *       1 IMSBR_A_CALL_STATE  (u32)   2 IMSBR_A_TUPLE (struct imsbr_tuple)
 *       3 IMSBR_A_SIMCARD    (u32)   4 IMSBR_A_LOCALMAC (string)
 *       5 IMSBR_A_REMOTEADDR (inet addr)  6 IMSBR_A_ISV4 (u32)
 *       7 IMSBR_A_LOWPOWER_ST(u32)   8 IMSBR_A_ESP_SPI (u32)
 *     struct imsbr_tuple = { local_addr[16], peer_addr[16], local_port,
 *                            peer_port, l3proto, l4proto, media_type,
 *                            link_type, socket_type, sim_card }
 *   kernel module params used by the Android daemon:
 *     /sys/module/ims_bridge/parameters/tcpdump_enable
 *     /sys/module/ims_bridge/parameters/vowifi_in_mark
 *
 * Unlike the Android daemon (libnl + Android log), this tool talks to the
 * generic netlink kernel API directly with plain syscalls, so it runs on a
 * bare Linux userspace.
 *
 * Build (aarch64 Linux, static):
 *   aarch64-none-linux-gnu-gcc -O2 -static -o unisoc-ims-bridge unisoc-ims-bridge.c
 *
 * Usage:
 *   unisoc-ims-bridge list
 *   unisoc-ims-bridge call-state <sim> <state>
 *   unisoc-ims-bridge lp-state <sim> <start|end>
 *   unisoc-ims-bridge spi-add <sim> <spi>
 *   unisoc-ims-bridge spi-del <sim> <spi>
 *   unisoc-ims-bridge local-mac <sim> <mac>
 *   unisoc-ims-bridge remote-mac <sim> <ipv4> <isv4>
 *   unisoc-ims-bridge params
 *   unisoc-ims-bridge help
 */

#define _GNU_SOURCE
#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * The vendor toolchain ships an older <linux/genetlink.h> without
 * GENL_VERSION, and has no libnl (nla_len/NLA_DATA). Define the few
 * helpers we need; values match the kernel uapi headers of the 6.18
 * VXTux port (include/uapi/linux/{genetlink,netlink}.h).
 */
#ifndef GENL_VERSION
#define GENL_VERSION	0x02
#endif

#ifndef NLA_HDRLEN
#define NLA_HDRLEN	((int)NLA_ALIGN(sizeof(struct nlattr)))
#endif

static inline int nla_len(const struct nlattr *nla)
{
	return (int)nla->nla_len;
}

static inline void *NLA_DATA(const struct nlattr *nla)
{
	return (void *)((const char *)nla + NLA_HDRLEN);
}

/* --- ABI from the 6.18 VXTux kernel port --------------------------------- */
#define IMSBR_GENL_NAME		"imsbr"
#define IMSBR_GENL_VERSION	1

enum imsbr_commands {
	IMSBR_C_UNSPEC		= 0,
	IMSBR_C_CALL_STATE	= 1,
	IMSBR_C_ADD_TUPLE	= 2,
	IMSBR_C_DEL_TUPLE	= 3,
	IMSBR_C_RESET_TUPLE	= 4,
	IMSBR_C_SEND_LOCALMAC	= 5,
	IMSBR_C_SEND_REMOTEMAC	= 6,
	IMSBR_C_LOWPOWER_ST	= 7,
	IMSBR_C_ADD_SPI		= 8,
	IMSBR_C_DEL_SPI		= 9,
};

enum imsbr_attrs {
	IMSBR_A_UNSPEC		= 0,
	IMSBR_A_CALL_STATE	= 1,
	IMSBR_A_TUPLE		= 2,
	IMSBR_A_SIMCARD		= 3,
	IMSBR_A_LOCALMAC	= 4,
	IMSBR_A_REMOTEADDR	= 5,
	IMSBR_A_ISV4		= 6,
	IMSBR_A_LOWPOWER_ST	= 7,
	IMSBR_A_ESP_SPI		= 8,
};

enum imsbr_lowpower_state {
	IMSBR_LOWPOWER_UNSPEC	= 0,
	IMSBR_LOWPOWER_START	= 1,
	IMSBR_LOWPOWER_END	= 2,
};

/* union imsbr_inet_addr { u32 all[4]; ... } = 16 bytes */
struct imsbr_inet_addr {
	uint32_t all[4];
};

static int inet_pton_af(const char *ip, unsigned isv4,
			struct imsbr_inet_addr *out);

/* struct imsbr_tuple - byte exact copy of the kernel struct */
struct imsbr_tuple {
	struct imsbr_inet_addr local_addr;
	struct imsbr_inet_addr peer_addr;
	uint16_t local_port;	/* __be16 */
	uint16_t peer_port;	/* __be16 */
	uint16_t l3proto;
	uint8_t  l4proto;
	uint8_t  media_type;
	uint16_t link_type;
	uint16_t socket_type;
	uint32_t sim_card;
};

#define IMSBR_PARAM_TCPDUMP	"/sys/module/ims_bridge/parameters/tcpdump_enable"
#define IMSBR_PARAM_VOWIFI_MARK	"/sys/module/ims_bridge/parameters/vowifi_in_mark"

struct nl_ctx {
	int fd;
	uint32_t family_id;
	uint32_t seq;
};

static int nl_open(struct nl_ctx *c)
{
	struct sockaddr_nl sa;

	memset(c, 0, sizeof(*c));
	c->fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
	if (c->fd < 0) {
		fprintf(stderr, "unisoc-ims-bridge: netlink socket: %s\n",
			strerror(errno));
		return -1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	if (bind(c->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		fprintf(stderr, "unisoc-ims-bridge: bind: %s\n", strerror(errno));
		close(c->fd);
		c->fd = -1;
		return -1;
	}

	return 0;
}

/* Resolve the "imsbr" generic netlink family id. */
static int nl_resolve_family(struct nl_ctx *c)
{
	struct {
		struct nlmsghdr nh;
		struct genlmsghdr gh;
		char attrs[128];
	} req;
	struct nlmsghdr *nh;
	char rx[4096];
	int ret, alen;

	memset(&req, 0, sizeof(req));
	req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct genlmsghdr));
	req.nh.nlmsg_type = GENL_ID_CTRL;
	req.nh.nlmsg_flags = NLM_F_REQUEST;
	req.nh.nlmsg_seq = ++c->seq;
	req.gh.version = GENL_VERSION;
	req.gh.cmd = CTRL_CMD_GETFAMILY;

	/*
	 * CTRL_CMD_GETFAMILY expects the family name inside a
	 * CTRL_ATTR_FAMILY_NAME netlink attribute (a bare string yields
	 * -EINVAL from the kernel).
	 */
	alen = NLA_HDRLEN + (int)strlen(IMSBR_GENL_NAME) + 1;
	{
		struct nlattr *na = (struct nlattr *)req.attrs;
		na->nla_type = CTRL_ATTR_FAMILY_NAME;
		na->nla_len = (unsigned short)alen;
		memcpy(NLA_DATA(na), IMSBR_GENL_NAME, strlen(IMSBR_GENL_NAME) + 1);
	}
	req.nh.nlmsg_len += NLA_ALIGN(alen);

	if (send(c->fd, &req, req.nh.nlmsg_len, 0) < 0) {
		fprintf(stderr, "unisoc-ims-bridge: send GETFAMILY: %s\n",
			strerror(errno));
		return -1;
	}

	ret = (int)recv(c->fd, rx, sizeof(rx), 0);
	if (ret < 0) {
		fprintf(stderr, "unisoc-ims-bridge: recv GETFAMILY: %s\n",
			strerror(errno));
		return -1;
	}

	nh = (struct nlmsghdr *)rx;
	if (nh->nlmsg_type == NLMSG_ERROR) {
		struct nlmsgerr *e = NLMSG_DATA(nh);
		fprintf(stderr, "unisoc-ims-bridge: family '%s' not available "
			"(kernel error %d)\n", IMSBR_GENL_NAME, e->error);
		return -1;
	}

	if (nh->nlmsg_type != GENL_ID_CTRL) {
		fprintf(stderr, "unisoc-ims-bridge: unexpected reply type %u\n",
			nh->nlmsg_type);
		return -1;
	}

	{
		struct genlmsghdr *gh = NLMSG_DATA(nh);
		struct nlattr *attrs = (struct nlattr *)((char *)gh + GENL_HDRLEN);
		int rem = nh->nlmsg_len - NLMSG_LENGTH(sizeof(*gh));

		while (rem >= (int)sizeof(struct nlattr)) {
			int alen = nla_len(attrs);
			if (alen < 0 || alen > rem)
				break;
			if (attrs->nla_type == CTRL_ATTR_FAMILY_ID) {
				c->family_id = *(uint32_t *)NLA_DATA(attrs);
				return 0;
			}
			attrs = (struct nlattr *)((char *)attrs + NLA_ALIGN(alen));
			rem -= NLA_ALIGN(alen);
		}
	}

	fprintf(stderr, "unisoc-ims-bridge: family id not found in reply\n");
	return -1;
}

static int nl_doit(struct nl_ctx *c, int cmd, struct nlattr *attrs, int attrlen)
{
	struct {
		struct nlmsghdr nh;
		struct genlmsghdr gh;
		char attrs[512];
	} req;
	char rx[4096];
	int ret;

	if (attrlen > (int)sizeof(req.attrs)) {
		fprintf(stderr, "unisoc-ims-bridge: attrs too big\n");
		return -1;
	}

	memset(&req, 0, sizeof(req));
	req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(struct genlmsghdr)) + attrlen;
	req.nh.nlmsg_type = c->family_id;
	req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	req.nh.nlmsg_seq = ++c->seq;
	req.gh.version = IMSBR_GENL_VERSION;
	req.gh.cmd = cmd;
	if (attrlen)
		memcpy(req.attrs, attrs, attrlen);

	if (send(c->fd, &req, req.nh.nlmsg_len, 0) < 0) {
		fprintf(stderr, "unisoc-ims-bridge: send: %s\n", strerror(errno));
		return -1;
	}

	ret = (int)recv(c->fd, rx, sizeof(rx), 0);
	if (ret < 0) {
		fprintf(stderr, "unisoc-ims-bridge: recv: %s\n", strerror(errno));
		return -1;
	}

	{
		struct nlmsghdr *nh = (struct nlmsghdr *)rx;
		if (nh->nlmsg_type == NLMSG_ERROR) {
			struct nlmsgerr *e = NLMSG_DATA(nh);
			if (e->error == 0) {
				printf("OK: command %d accepted\n", cmd);
				return 0;
			}
			fprintf(stderr, "unisoc-ims-bridge: command %d rejected: %s\n",
				cmd, strerror(-e->error));
			return -1;
		}
	}

	printf("OK: command %d accepted\n", cmd);
	return 0;
}

static int cmd_call_state(struct nl_ctx *c, unsigned sim, unsigned state)
{
	char attrs[128];
	struct nlattr *na = (struct nlattr *)attrs;
	int len = 0;

	na->nla_type = IMSBR_A_SIMCARD;
	na->nla_len = NLA_HDRLEN + sizeof(uint32_t);
	memcpy(NLA_DATA(na), &sim, sizeof(sim));
	len += NLA_ALIGN(na->nla_len);

	na = (struct nlattr *)(attrs + len);
	na->nla_type = IMSBR_A_CALL_STATE;
	na->nla_len = NLA_HDRLEN + sizeof(uint32_t);
	memcpy(NLA_DATA(na), &state, sizeof(state));
	len += NLA_ALIGN(na->nla_len);

	return nl_doit(c, IMSBR_C_CALL_STATE, (struct nlattr *)attrs, len);
}

static int cmd_lp_state(struct nl_ctx *c, unsigned sim, unsigned state)
{
	char attrs[128];
	struct nlattr *na = (struct nlattr *)attrs;
	int len = 0;

	na->nla_type = IMSBR_A_SIMCARD;
	na->nla_len = NLA_HDRLEN + sizeof(uint32_t);
	memcpy(NLA_DATA(na), &sim, sizeof(sim));
	len += NLA_ALIGN(na->nla_len);

	na = (struct nlattr *)(attrs + len);
	na->nla_type = IMSBR_A_LOWPOWER_ST;
	na->nla_len = NLA_HDRLEN + sizeof(uint32_t);
	memcpy(NLA_DATA(na), &state, sizeof(state));
	len += NLA_ALIGN(na->nla_len);

	return nl_doit(c, IMSBR_C_LOWPOWER_ST, (struct nlattr *)attrs, len);
}

static int cmd_spi(struct nl_ctx *c, int cmd, unsigned sim, unsigned spi)
{
	char attrs[128];
	struct nlattr *na = (struct nlattr *)attrs;
	int len = 0;

	na->nla_type = IMSBR_A_SIMCARD;
	na->nla_len = NLA_HDRLEN + sizeof(uint32_t);
	memcpy(NLA_DATA(na), &sim, sizeof(sim));
	len += NLA_ALIGN(na->nla_len);

	na = (struct nlattr *)(attrs + len);
	na->nla_type = IMSBR_A_ESP_SPI;
	na->nla_len = NLA_HDRLEN + sizeof(uint32_t);
	memcpy(NLA_DATA(na), &spi, sizeof(spi));
	len += NLA_ALIGN(na->nla_len);

	return nl_doit(c, cmd, (struct nlattr *)attrs, len);
}

static int cmd_local_mac(struct nl_ctx *c, unsigned sim, const char *mac)
{
	char attrs[256];
	struct nlattr *na = (struct nlattr *)attrs;
	int len = 0;

	na->nla_type = IMSBR_A_SIMCARD;
	na->nla_len = NLA_HDRLEN + sizeof(uint32_t);
	memcpy(NLA_DATA(na), &sim, sizeof(sim));
	len += NLA_ALIGN(na->nla_len);

	na = (struct nlattr *)(attrs + len);
	na->nla_type = IMSBR_A_LOCALMAC;
	na->nla_len = NLA_HDRLEN + (int)strlen(mac) + 1;
	memcpy(NLA_DATA(na), mac, strlen(mac) + 1);
	len += NLA_ALIGN(na->nla_len);

	return nl_doit(c, IMSBR_C_SEND_LOCALMAC, (struct nlattr *)attrs, len);
}

static int cmd_remote_mac(struct nl_ctx *c, unsigned sim, const char *ip,
			  unsigned isv4)
{
	char attrs[256];
	struct nlattr *na = (struct nlattr *)attrs;
	struct imsbr_inet_addr addr;
	int len = 0;

	memset(&addr, 0, sizeof(addr));
	if (inet_pton_af(ip, isv4, &addr) < 0) {
		fprintf(stderr, "unisoc-ims-bridge: bad address %s\n", ip);
		return -1;
	}

	na->nla_type = IMSBR_A_SIMCARD;
	na->nla_len = NLA_HDRLEN + sizeof(uint32_t);
	memcpy(NLA_DATA(na), &sim, sizeof(sim));
	len += NLA_ALIGN(na->nla_len);

	na = (struct nlattr *)(attrs + len);
	na->nla_type = IMSBR_A_REMOTEADDR;
	na->nla_len = NLA_HDRLEN + (int)sizeof(addr);
	memcpy(NLA_DATA(na), &addr, sizeof(addr));
	len += NLA_ALIGN(na->nla_len);

	na = (struct nlattr *)(attrs + len);
	na->nla_type = IMSBR_A_ISV4;
	na->nla_len = NLA_HDRLEN + sizeof(uint32_t);
	memcpy(NLA_DATA(na), &isv4, sizeof(isv4));
	len += NLA_ALIGN(na->nla_len);

	return nl_doit(c, IMSBR_C_SEND_REMOTEMAC, (struct nlattr *)attrs, len);
}

/* Minimal IPv4/IPv6 parser to avoid pulling libnl/inet_pton. */
static int inet_pton_af(const char *ip, unsigned isv4,
			 struct imsbr_inet_addr *out)
{
	unsigned a, b, c, d;

	if (isv4) {
		if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
			return -1;
		if (a > 255 || b > 255 || c > 255 || d > 255)
			return -1;
		out->all[0] = (a << 24) | (b << 16) | (c << 8) | d;
		return 0;
	}

	/* IPv6: only the plain hex form is accepted. */
	{
		unsigned w[8];
		int i;
		if (sscanf(ip, "%x:%x:%x:%x:%x:%x:%x:%x",
			   &w[0], &w[1], &w[2], &w[3],
			   &w[4], &w[5], &w[6], &w[7]) != 8)
			return -1;
		for (i = 0; i < 8; i++) {
			if (w[i] > 0xffff)
				return -1;
			out->all[i / 2] |= (i & 1) ? w[i] : (w[i] << 16);
		}
	}
	return 0;
}

static void show_param(const char *path)
{
	FILE *f = fopen(path, "r");
	char buf[64];

	if (!f) {
		printf("%-46s : <unavailable: %s>\n", path, strerror(errno));
		return;
	}
	if (!fgets(buf, sizeof(buf), f))
		buf[0] = '\0';
	fclose(f);
	buf[strcspn(buf, "\r\n")] = '\0';
	printf("%-46s : %s\n", path, buf);
}

static int cmd_params(void)
{
	show_param(IMSBR_PARAM_TCPDUMP);
	show_param(IMSBR_PARAM_VOWIFI_MARK);
	return 0;
}

static int cmd_list(struct nl_ctx *c)
{
	printf("imsbr generic netlink family: %s (v%d), id=%u\n",
	       IMSBR_GENL_NAME, IMSBR_GENL_VERSION, c->family_id);
	printf("commands: call-state=%d add-tuple=%d del-tuple=%d reset-tuple=%d\n",
	       IMSBR_C_CALL_STATE, IMSBR_C_ADD_TUPLE, IMSBR_C_DEL_TUPLE,
	       IMSBR_C_RESET_TUPLE);
	printf("          local-mac=%d remote-mac=%d lp-state=%d spi-add=%d spi-del=%d\n",
	       IMSBR_C_SEND_LOCALMAC, IMSBR_C_SEND_REMOTEMAC,
	       IMSBR_C_LOWPOWER_ST, IMSBR_C_ADD_SPI, IMSBR_C_DEL_SPI);
	return 0;
}

static void usage(const char *a0)
{
	printf(
"unisoc-ims-bridge - Unisoc IMS bridge control (Linux userspace)\n"
"\n"
"Usage:\n"
"  %s list                       show family id + supported commands\n"
"  %s params                     show ims_bridge kernel module parameters\n"
"  %s call-state <sim> <state>   set IMS call state\n"
"  %s lp-state <sim> <start|end> set IMS low power state\n"
"  %s spi-add <sim> <spi>        register ESP SPI\n"
"  %s spi-del <sim> <spi>        remove ESP SPI\n"
"  %s local-mac <sim> <mac>      announce local MAC\n"
"  %s remote-mac <sim> <ip> <4|6> announce remote address\n"
"  %s help\n",
		a0, a0, a0, a0, a0, a0, a0, a0, a0);
}

int main(int argc, char **argv)
{
	struct nl_ctx ctx;
	int rc = 0;

	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}

	if (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help") ||
	    !strcmp(argv[1], "-h")) {
		usage(argv[0]);
		return 0;
	}

	if (!strcmp(argv[1], "params"))
		return cmd_params();

	if (nl_open(&ctx) < 0)
		return 1;
	if (nl_resolve_family(&ctx) < 0) {
		close(ctx.fd);
		return 1;
	}

	if (!strcmp(argv[1], "list")) {
		rc = cmd_list(&ctx);
	} else if (!strcmp(argv[1], "call-state") && argc == 4) {
		rc = cmd_call_state(&ctx, (unsigned)atoi(argv[2]),
				    (unsigned)atoi(argv[3]));
	} else if (!strcmp(argv[1], "lp-state") && argc == 4) {
		unsigned st = !strcmp(argv[3], "end") ? IMSBR_LOWPOWER_END
			   : !strcmp(argv[3], "start") ? IMSBR_LOWPOWER_START
			   : IMSBR_LOWPOWER_UNSPEC;
		rc = cmd_lp_state(&ctx, (unsigned)atoi(argv[2]), st);
	} else if (!strcmp(argv[1], "spi-add") && argc == 4) {
		rc = cmd_spi(&ctx, IMSBR_C_ADD_SPI, (unsigned)atoi(argv[2]),
			     (unsigned)strtoul(argv[3], NULL, 0));
	} else if (!strcmp(argv[1], "spi-del") && argc == 4) {
		rc = cmd_spi(&ctx, IMSBR_C_DEL_SPI, (unsigned)atoi(argv[2]),
			     (unsigned)strtoul(argv[3], NULL, 0));
	} else if (!strcmp(argv[1], "local-mac") && argc == 4) {
		rc = cmd_local_mac(&ctx, (unsigned)atoi(argv[2]), argv[3]);
	} else if (!strcmp(argv[1], "remote-mac") && argc == 5) {
		rc = cmd_remote_mac(&ctx, (unsigned)atoi(argv[2]), argv[3],
				    (unsigned)atoi(argv[4]));
	} else {
		fprintf(stderr, "unisoc-ims-bridge: bad usage\n");
		usage(argv[0]);
		rc = 2;
	}

	close(ctx.fd);
	return rc < 0 ? 1 : rc;
}
