/*
 * rwdhcpd: the hotspot's DHCP server (A, 2026-11-19). iOS's bootpd exits at once when not started by Personal Hotspot,
 * so the S20's access point hands out addresses with this: 172.20.10.2-14 on IF (the phone is 172.20.10.1, as on an
 * iPhone hotspot), router 172.20.10.1, DNS as given.
 *
 *   rwdhcpd IF [DNS]          e.g. rwdhcpd en0 8.8.8.8
 *
 * - Only requests that arrive on IF are answered (not the USB link to a computer, say).
 * - Replies go out as raw Ethernet frames through BPF, as iOS's bootpd does: a UDP socket gets EHOSTUNREACH for
 *   255.255.255.255 here, and clients ignore an offer to the subnet broadcast.
 * - Leases (by MAC, 1 hour) are kept in /var/db/rwdhcpd.leases, so a restart of the phone or of this server gives a
 *   client back its address instead of handing it to the next one (two clients on one address, seen 2026-11-19).
 *   An expired lease is reused only when no free address is left.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define BIOCSETIF _IOW('B', 108, struct ifreq)      /* net/bpf.h, absent from the iOS SDK */

#define NET     0xac140a00u     /* 172.20.10.0/28 */
#define SELF    (NET | 1)
#define FIRST   2
#define LAST    14
#define LEASE   3600
#define LEASES  "/var/db/rwdhcpd.leases"

static struct lease { uint8_t mac[6]; uint8_t pad[2]; int64_t until; } leases[LAST + 1];
static int bpf = -1;
static uint8_t self_mac[6];

static void
leases_load(void)
{
	FILE *f = fopen(LEASES, "rb");
	if (f) {
		if (fread(leases, sizeof(leases), 1, f) != 1) {
			memset(leases, 0, sizeof(leases));
		}
		fclose(f);
	}
}

static void
leases_save(void)
{
	FILE *f = fopen(LEASES ".new", "wb");
	if (f && fwrite(leases, sizeof(leases), 1, f) == 1 && fclose(f) == 0) {
		rename(LEASES ".new", LEASES);
	} else if (f) {
		fclose(f);
	}
}

/* the client's address: its own lease, else a never-used one, else the longest-expired one */
static int
pick(const uint8_t *mac, int64_t now)
{
	static const uint8_t zero[6];
	int fresh = 0, stale = 0;

	for (int i = FIRST; i <= LAST; i++) {
		if (!memcmp(leases[i].mac, mac, 6)) {
			return i;
		}
		if (!fresh && !memcmp(leases[i].mac, zero, 6)) {
			fresh = i;
		}
		if (leases[i].until < now && (!stale || leases[i].until < leases[stale].until)) {
			stale = i;
		}
	}
	int h = fresh ? fresh : stale;
	if (h) {
		memcpy(leases[h].mac, mac, 6);
		leases[h].until = 0;
	}
	return h;
}

static uint8_t *
opt(uint8_t *o, uint8_t code, uint8_t len, const void *data)
{
	*o++ = code;
	*o++ = len;
	memcpy(o, data, len);
	return o + len;
}

static int
bpf_open(const char *ifname)
{
	char dev[16];
	for (int i = 0; i < 64 && bpf < 0; i++) {
		snprintf(dev, sizeof(dev), "/dev/bpf%d", i);
		bpf = open(dev, O_WRONLY);
	}
	if (bpf < 0) {
		return -1;
	}
	struct ifreq ifr = { 0 };
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	if (ioctl(bpf, BIOCSETIF, &ifr) < 0) {
		close(bpf);
		bpf = -1;
		return -1;
	}
	struct ifaddrs *ia, *i;
	if (getifaddrs(&ia) == 0) {
		for (i = ia; i; i = i->ifa_next) {
			if (i->ifa_addr && i->ifa_addr->sa_family == AF_LINK && !strcmp(i->ifa_name, ifname)) {
				memcpy(self_mac, LLADDR((struct sockaddr_dl *)i->ifa_addr), 6);
			}
		}
		freeifaddrs(ia);
	}
	return 0;
}

static uint16_t
csum(const uint8_t *d, size_t n)
{
	uint32_t s = 0;
	for (size_t i = 0; i + 1 < n; i += 2) {
		s += (uint32_t)(d[i] << 8 | d[i + 1]);
	}
	if (n & 1) {
		s += (uint32_t)d[n - 1] << 8;
	}
	while (s >> 16) {
		s = (s & 0xffff) + (s >> 16);
	}
	return (uint16_t)~s;
}

/* Ethernet to ff:ff..., IPv4 SELF -> 255.255.255.255, UDP 67 -> 68 (UDP checksum 0: allowed for IPv4) */
static ssize_t
send_raw(const uint8_t *dhcp, size_t n)
{
	uint8_t f[1600];
	if (n + 42 > sizeof(f)) {
		errno = EMSGSIZE;
		return -1;
	}
	memset(f, 0xff, 6);
	memcpy(f + 6, self_mac, 6);
	f[12] = 0x08;
	f[13] = 0x00;
	uint8_t *ip = f + 14, *udp = ip + 20;
	uint16_t tl = (uint16_t)(20 + 8 + n), ul = (uint16_t)(8 + n);
	memset(ip, 0, 20);
	ip[0] = 0x45;
	ip[2] = tl >> 8;
	ip[3] = tl & 0xff;
	ip[8] = 64;
	ip[9] = 17;
	uint32_t src = htonl(SELF), dst = 0xffffffffu;
	memcpy(ip + 12, &src, 4);
	memcpy(ip + 16, &dst, 4);
	uint16_t c = csum(ip, 20);
	ip[10] = c >> 8;
	ip[11] = c & 0xff;
	udp[0] = 0; udp[1] = 67; udp[2] = 0; udp[3] = 68;
	udp[4] = ul >> 8; udp[5] = ul & 0xff; udp[6] = 0; udp[7] = 0;
	memcpy(udp + 8, dhcp, n);
	return write(bpf, f, 42 + n);
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: rwdhcpd IF [DNS]\n");
		return 2;
	}
	uint32_t dns = htonl(SELF);
	if (argc > 2 && inet_pton(AF_INET, argv[2], &dns) != 1) {
		fprintf(stderr, "rwdhcpd: bad DNS address %s\n", argv[2]);
		return 2;
	}
	unsigned idx = if_nametoindex(argv[1]);
	if (idx == 0) {
		fprintf(stderr, "rwdhcpd: no interface %s\n", argv[1]);
		return 1;
	}
	int s = socket(AF_INET, SOCK_DGRAM, 0), one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	setsockopt(s, IPPROTO_IP, IP_RECVIF, &one, sizeof(one));
	struct sockaddr_in sa = { .sin_len = sizeof(sa), .sin_family = AF_INET, .sin_port = htons(67) };
	if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		perror("rwdhcpd: bind :67");
		return 1;
	}
	if (bpf_open(argv[1]) < 0) {
		perror("rwdhcpd: bpf");
		return 1;
	}
	leases_load();
	printf("rwdhcpd: on %s (index %u), 172.20.10.%d-%d\n", argv[1], idx, FIRST, LAST);
	fflush(stdout);
	for (;;) {
		uint8_t p[1500], cbuf[64];
		struct iovec iov = { p, sizeof(p) };
		struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof(cbuf) };
		ssize_t n = recvmsg(s, &mh, 0);
		unsigned rif = 0;
		for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
			if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_RECVIF) {
				rif = ((struct sockaddr_dl *)CMSG_DATA(c))->sdl_index;
			}
		}
		/* BOOTREQUEST over Ethernet (htype 1, hlen 6) with the DHCP magic cookie, on IF only */
		if (rif != idx || n < 240 || p[0] != 1 || p[1] != 1 || p[2] != 6 ||
		    p[236] != 99 || p[237] != 130 || p[238] != 83 || p[239] != 99) {
			continue;
		}
		uint8_t type = 0, *mac = p + 28;
		uint32_t want = 0, server = 0;
		for (ssize_t i = 240; i + 1 < n && p[i] != 255;) {
			if (p[i] == 0) {
				i++;
				continue;
			}
			if (i + 2 + p[i + 1] > n) {
				break;
			}
			if (p[i] == 53 && p[i + 1] >= 1) {
				type = p[i + 2];
			} else if (p[i] == 50 && p[i + 1] == 4) {
				memcpy(&want, p + i + 2, 4);
			} else if (p[i] == 54 && p[i + 1] == 4) {
				memcpy(&server, p + i + 2, 4);
			}
			i += 2 + p[i + 1];
		}
		if (type == 3 && server && server != htonl(SELF)) {
			continue;                       /* a REQUEST that chose another server's offer */
		}
		if (type == 3 && !want) {               /* RENEWING/REBINDING: the address is in ciaddr */
			memcpy(&want, p + 12, 4);
		}
		if (type != 1 && type != 3) {           /* only DISCOVER and REQUEST need an answer */
			continue;
		}
		int64_t now = time(NULL);
		int host = pick(mac, now);
		if (!host) {
			printf("rwdhcpd: no free address\n");
			fflush(stdout);
			continue;
		}
		uint32_t yi = htonl(NET | (uint32_t)host), self = htonl(SELF), mask = htonl(0xfffffff0), lt = htonl(LEASE);
		bool nak = type == 3 && want && want != yi;
		if (type == 3 && !nak) {
			leases[host].until = now + LEASE;
			leases_save();
		}
		p[0] = 2;                               /* BOOTREPLY; xid, flags, chaddr kept */
		memset(p + 12, 0, 4);                   /* ciaddr */
		memcpy(p + 16, nak ? (uint8_t[4]){ 0 } : (uint8_t *)&yi, 4);
		memcpy(p + 20, &self, 4);
		uint8_t *o = p + 240, t = nak ? 6 : type == 1 ? 2 : 5;  /* NAK, OFFER, ACK */
		o = opt(o, 53, 1, &t);
		o = opt(o, 54, 4, &self);
		if (!nak) {
			o = opt(o, 51, 4, &lt);
			o = opt(o, 1, 4, &mask);
			o = opt(o, 3, 4, &self);
			o = opt(o, 6, 4, &dns);
		}
		*o++ = 255;
		ssize_t w = send_raw(p, (size_t)(o - p));
		printf("rwdhcpd: %s %02x:%02x:%02x:%02x:%02x:%02x -> %s 172.20.10.%d (%s)\n", type == 1 ? "DISCOVER" : "REQUEST",
		    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], nak ? "NAK" : type == 1 ? "OFFER" : "ACK", host,
		    w < 0 ? strerror(errno) : "sent");
		fflush(stdout);
	}
}
