/*
 * clat-test: switch on iOS's own CLAT46 (bsd/net/nat464_utils.c) on the cellular tunnel, by hand, for the hotspot
 * bring-up (2026-11-19). Jio is IPv6-only: IPv4 from apps and from hotspot clients must be translated to IPv6 and sent
 * to the carrier's NAT64. On a real iPhone configd/CommCenter do these steps; here nobody does.
 *
 *   clat-test nat64 [DNS]           AAAA of ipv4only.arpa (RFC 7050): the carrier's NAT64 prefix
 *   clat-test start IF PREFIX/96    set the NAT64 prefix, SIOCCLAT46_START, print the CLAT46 address (IF's prefix
 *                                   must be autonomous: the cellular service adds its address as autoconf)
 *   clat-test stop IF               SIOCCLAT46_STOP
 */
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

/* private kernel ABI (xnu-8796.121.2 bsd/netinet6/in6_var.h, bsd/net/if_private.h) */
#define IN6_IFF_AUTOCONF        0x0040
#define NAT64_MAX_NUM_PREFIXES  4
struct rw_lifetime { time_t ia6t_expire, ia6t_preferred; uint32_t ia6t_vltime, ia6t_pltime; };
struct rw_aliasreq { char name[IFNAMSIZ]; struct sockaddr_in6 addr, dst, mask; int flags; struct rw_lifetime lt; };
struct rw_ifreq6 { char name[IFNAMSIZ]; union { struct sockaddr_in6 a; int flags; char pad[272]; } u; };   /* sizeof(struct in6_ifreq) == 288 */
struct rw_prefix { struct in6_addr p; uint32_t len; };
struct rw_nat64req { char name[IFNAMSIZ]; struct rw_prefix p[NAT64_MAX_NUM_PREFIXES]; };
struct rw_clat46req { char name[IFNAMSIZ]; struct { struct in6_addr a; uint32_t len; } a6; };
#define RW_SIOCAIFADDR_IN6      _IOW('i', 26, struct rw_aliasreq)
#define RW_SIOCDIFADDR_IN6      _IOW('i', 25, struct rw_ifreq6)
#define RW_SIOCCLAT46_START     _IOWR('i', 189, struct rw_ifreq6)
#define RW_SIOCCLAT46_STOP      _IOWR('i', 190, struct rw_ifreq6)
#define RW_SIOCSIFNAT64PREFIX   _IOWR('i', 194, struct rw_nat64req)
#define RW_SIOCGIFCLAT46ADDR    _IOWR('i', 201, struct rw_clat46req)

/* AAAA ipv4only.arpa straight to a DNS server (the carrier's; the system resolver may not be using it) */
static int
nat64_at(const char *server)
{
	static const uint8_t q[] = { 0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 8, 'i', 'p', 'v', '4', 'o', 'n', 'l', 'y', 4, 'a', 'r',
		'p', 'a', 0, 0, 28, 0, 1 };
	struct sockaddr_in6 sa = { .sin6_len = sizeof(sa), .sin6_family = AF_INET6, .sin6_port = htons(53) };
	uint8_t r[512];
	struct timeval tv = { 3, 0 };
	int s = socket(AF_INET6, SOCK_DGRAM, 0);
	inet_pton(AF_INET6, server, &sa.sin6_addr);
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (sendto(s, q, sizeof(q), 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		printf("send to %s: %s\n", server, strerror(errno));
		return 1;
	}
	ssize_t n = recv(s, r, sizeof(r), 0);
	if (n < 12) {
		printf("no answer from %s (%s)\n", server, n < 0 ? strerror(errno) : "short");
		return 1;
	}
	int an = r[6] << 8 | r[7], found = 0;
	/* AAAA records are 16-byte rdata: scan for type 28 class 1 ... rdlength 16 */
	for (ssize_t i = sizeof(q); i + 26 <= n; i++) {
		if (r[i] == 0 && r[i + 1] == 28 && r[i + 2] == 0 && r[i + 3] == 1 && r[i + 8] == 0 && r[i + 9] == 16) {
			char a[64];
			inet_ntop(AF_INET6, r + i + 10, a, sizeof(a));
			printf("ipv4only.arpa AAAA %s\n", a);
			found++;
			i += 25;
		}
	}
	printf("%d answers, %d AAAA\n", an, found);
	return !found;
}

static int
nat64(void)
{
	struct addrinfo hints = { .ai_family = AF_INET6, .ai_socktype = SOCK_DGRAM }, *r, *a;
	int e = getaddrinfo("ipv4only.arpa", NULL, &hints, &r);
	if (e) {
		printf("ipv4only.arpa: %s\n", gai_strerror(e));
		return 1;
	}
	for (a = r; a; a = a->ai_next) {
		char s[64];
		inet_ntop(AF_INET6, &((struct sockaddr_in6 *)a->ai_addr)->sin6_addr, s, sizeof(s));
		printf("ipv4only.arpa AAAA %s\n", s);
	}
	freeaddrinfo(r);
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "nat64")) {
		return argc >= 3 ? nat64_at(argv[2]) : nat64();
	}
	if (argc < 3) {
		fprintf(stderr, "usage: clat-test nat64 | start IF PREFIX/96 | stop IF\n");
		return 2;
	}
	int s = socket(AF_INET6, SOCK_DGRAM, 0);
	struct rw_ifreq6 ir = { 0 };
	strlcpy(ir.name, argv[2], sizeof(ir.name));
	if (!strcmp(argv[1], "stop")) {
		printf("CLAT46_STOP: %s\n", ioctl(s, RW_SIOCCLAT46_STOP, &ir) ? strerror(errno) : "ok");
		return 0;
	}
	if (strcmp(argv[1], "start") || argc < 4) {
		return 2;
	}
	/* the cellular service adds its address as autoconf (live-modem.c tun_open), so the /64 prefix is autonomous,
	 * which CLAT46 needs; check with ndp -p (flags A...) */
	/* 2. the NAT64 prefix */
	struct rw_nat64req nr = { 0 };
	char pfx[64];
	strlcpy(nr.name, argv[2], sizeof(nr.name));
	strlcpy(pfx, argv[3], sizeof(pfx));
	char *slash = strchr(pfx, '/');
	nr.p[0].len = (slash ? (uint32_t)atoi(slash + 1) : 96) / 8;      /* the kernel wants bytes (NAT64_PREFIX_LEN_96 == 12) */
	if (slash) {
		*slash = 0;
	}
	if (inet_pton(AF_INET6, pfx, &nr.p[0].p) != 1) {
		printf("bad prefix %s\n", argv[3]);
		return 1;
	}
	printf("SIFNAT64PREFIX %s/%u: %s\n", pfx, nr.p[0].len * 8, ioctl(s, RW_SIOCSIFNAT64PREFIX, &nr) ? strerror(errno) : "ok");
	/* 3. CLAT46 */
	printf("CLAT46_START: %s\n", ioctl(s, RW_SIOCCLAT46_START, &ir) ? strerror(errno) : "ok");
	struct rw_clat46req cr = { 0 };
	strlcpy(cr.name, argv[2], sizeof(cr.name));
	if (ioctl(s, RW_SIOCGIFCLAT46ADDR, &cr) == 0) {
		char a[64];
		inet_ntop(AF_INET6, &cr.a6.a, a, sizeof(a));
		printf("CLAT46 address %s/%u\n", a, cr.a6.len);
	} else {
		printf("GIFCLAT46ADDR: %s\n", strerror(errno));
	}
	return 0;
}
