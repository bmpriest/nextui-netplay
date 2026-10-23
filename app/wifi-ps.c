/* Wi-Fi power save, get and set, over raw nl80211.
 *
 *   netplay-wifips.elf IFACE            prints "on" or "off"
 *   netplay-wifips.elf IFACE on|off     sets it, then prints the result
 *
 * This is `iw dev IFACE get|set power_save` and nothing more. It exists because
 * the H700 rootfs ships no `iw`, `iwconfig` or `iwpriv`, and its 8821cs driver
 * rejects the wireless-extensions SIOCSIWPOWER ioctl - so without this the
 * radio stays in its driver default (rtw_power_mgnt=2, LPS max) and every input
 * packet waits for the next beacon wakeup: ~60 ms average, ~370 ms worst-case
 * round trips against ~13 ms with it off.
 *
 * Generic netlink by hand rather than libnl, which none of the images ship
 * either. Dependency-free so the same source builds on every platform. */
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>

struct msg { struct nlmsghdr n; struct genlmsghdr g; char buf[256]; };

static void put_attr(struct msg* m, int type, const void* data, int len) {
	struct nlattr* a = (struct nlattr*)((char*)m + NLMSG_ALIGN(m->n.nlmsg_len));
	a->nla_type = type;
	a->nla_len = NLA_HDRLEN + len;
	memcpy((char*)a + NLA_HDRLEN, data, len);
	m->n.nlmsg_len = NLMSG_ALIGN(m->n.nlmsg_len) + NLA_ALIGN(a->nla_len);
}

static void begin(struct msg* m, int type, int flags, int cmd) {
	memset(m, 0, sizeof(*m));
	m->n.nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	m->n.nlmsg_type = type;
	m->n.nlmsg_flags = NLM_F_REQUEST | flags;
	m->g.cmd = cmd;
	m->g.version = 1;
}

static int talk(int fd, struct msg* m, char* reply, int size) {
	static unsigned seq;
	m->n.nlmsg_seq = ++seq;
	if (send(fd, m, m->n.nlmsg_len, 0) < 0) return -1;
	int len = recv(fd, reply, size, 0);
	if (len < (int)sizeof(struct nlmsghdr)) return -1;
	struct nlmsghdr* h = (struct nlmsghdr*)reply;
	if (h->nlmsg_type == NLMSG_ERROR) {
		int err = ((struct nlmsgerr*)NLMSG_DATA(h))->error;
		if (err) { errno = -err; return -1; }
	}
	return len;
}

static struct nlattr* find_attr(char* reply, int type) {
	struct nlmsghdr* h = (struct nlmsghdr*)reply;
	char* p = (char*)NLMSG_DATA(h) + GENL_HDRLEN;
	char* end = (char*)h + h->nlmsg_len;
	while (p + NLA_HDRLEN <= end) {
		struct nlattr* a = (struct nlattr*)p;
		if (a->nla_len < NLA_HDRLEN) break;
		if ((a->nla_type & NLA_TYPE_MASK) == type) return a;
		p += NLA_ALIGN(a->nla_len);
	}
	return NULL;
}

int main(int argc, char** argv) {
	if (argc < 2 || argc > 3 ||
	    (argc == 3 && strcmp(argv[2], "on") && strcmp(argv[2], "off"))) {
		fprintf(stderr, "usage: %s IFACE [on|off]\n", argv[0]);
		return 2;
	}
	unsigned ifindex = if_nametoindex(argv[1]);
	if (!ifindex) { perror(argv[1]); return 1; }

	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
	struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
	if (fd < 0 || bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) { perror("netlink"); return 1; }
	char reply[8192];
	struct msg m;

	begin(&m, GENL_ID_CTRL, 0, CTRL_CMD_GETFAMILY);
	put_attr(&m, CTRL_ATTR_FAMILY_NAME, "nl80211", sizeof("nl80211"));
	if (talk(fd, &m, reply, sizeof(reply)) < 0) { perror("nl80211"); return 1; }
	struct nlattr* id = find_attr(reply, CTRL_ATTR_FAMILY_ID);
	if (!id) { fprintf(stderr, "no nl80211\n"); return 1; }
	unsigned short family = *(unsigned short*)((char*)id + NLA_HDRLEN);

	if (argc == 3) {
		unsigned state = strcmp(argv[2], "on") ? NL80211_PS_DISABLED : NL80211_PS_ENABLED;
		begin(&m, family, NLM_F_ACK, NL80211_CMD_SET_POWER_SAVE);
		put_attr(&m, NL80211_ATTR_IFINDEX, &ifindex, sizeof(ifindex));
		put_attr(&m, NL80211_ATTR_PS_STATE, &state, sizeof(state));
		if (talk(fd, &m, reply, sizeof(reply)) < 0) { perror("set power_save"); return 1; }
	}

	begin(&m, family, 0, NL80211_CMD_GET_POWER_SAVE);
	put_attr(&m, NL80211_ATTR_IFINDEX, &ifindex, sizeof(ifindex));
	if (talk(fd, &m, reply, sizeof(reply)) < 0) { perror("get power_save"); return 1; }
	struct nlattr* ps = find_attr(reply, NL80211_ATTR_PS_STATE);
	if (!ps) { fprintf(stderr, "no power save state\n"); return 1; }
	printf("%s\n", *(unsigned*)((char*)ps + NLA_HDRLEN) == NL80211_PS_ENABLED ? "on" : "off");
	return 0;
}
