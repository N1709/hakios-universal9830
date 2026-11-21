/*
 * rwpf: the hotspot's NAT, loaded straight through /dev/pf (A, 2026-11-19). iOS ships no pfctl; this does what
 *   pfctl -f with "scrub on EXT inet all max-mss 1400" + "nat on EXT inet from NET to any -> ADDR" does (replacing both
 *   rulesets),
 *   pf enabled, IPv4 forwarding on. The filter ruleset is left alone (empty = pass).
 *
 *   rwpf nat EXT ADDR NET/LEN     e.g. rwpf nat utun1 192.0.0.2 172.20.10.0/28
 *   rwpf flush                    empty NAT ruleset, forwarding off
 *
 * Built against the kernel's own bsd/net/pfvar.h (PRIVATE), as Apple's tools are: -I a folder holding net/pfvar.h,
 * net/radix.h and libkern/tree.h copied from xnu-src at build time (not kept in the repository).
 */
#define PRIVATE 1
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <net/pfvar.h>

static int dev;

/* one transaction over the scrub and the NAT ruleset: ticket[0] scrub, ticket[1] NAT */
static int
xact(unsigned long cmd, u_int32_t ticket[2])
{
	struct pfioc_trans_e e[2] = { { .rs_num = PF_RULESET_SCRUB }, { .rs_num = PF_RULESET_NAT } };
	struct pfioc_trans t = { .size = 2, .esize = sizeof(e[0]), .array = e };

	if (cmd == DIOCXCOMMIT) {
		e[0].ticket = ticket[0];
		e[1].ticket = ticket[1];
	}
	if (ioctl(dev, cmd, &t) < 0) {
		return -1;
	}
	ticket[0] = e[0].ticket;
	ticket[1] = e[1].ticket;
	return 0;
}

/* every rule gets a pool ticket (an empty pool for one that translates to nothing): DIOCBEGINADDRS */
static int
pool_begin(struct pfioc_pooladdr *pp)
{
	memset(pp, 0, sizeof(*pp));
	return ioctl(dev, DIOCBEGINADDRS, pp);
}

static void
forwarding(int on)
{
	(void)sysctlbyname("net.inet.ip.forwarding", NULL, NULL, &on, sizeof(on));
}

int
main(int argc, char **argv)
{
	u_int32_t ticket[2] = { 0, 0 };

	dev = open("/dev/pf", O_RDWR);
	if (dev < 0) {
		perror("rwpf: /dev/pf");
		return 1;
	}
	if (argc == 2 && !strcmp(argv[1], "flush")) {
		if (xact(DIOCXBEGIN, ticket) || xact(DIOCXCOMMIT, ticket)) {
			perror("rwpf: flush");
			return 1;
		}
		forwarding(0);
		printf("rwpf: scrub and NAT rules flushed, forwarding off\n");
		return 0;
	}
	if (argc != 5 || strcmp(argv[1], "nat")) {
		fprintf(stderr, "usage: rwpf nat EXT ADDR NET/LEN | rwpf flush\n");
		return 2;
	}
	struct in_addr addr, net;
	char nb[32];
	strlcpy(nb, argv[4], sizeof(nb));
	char *sl = strchr(nb, '/');
	int len = sl ? atoi(sl + 1) : 32;
	if (sl) {
		*sl = 0;
	}
	if (inet_pton(AF_INET, argv[3], &addr) != 1 || inet_pton(AF_INET, nb, &net) != 1 || len < 0 || len > 32) {
		fprintf(stderr, "rwpf: bad address\n");
		return 2;
	}

	if (xact(DIOCXBEGIN, ticket)) {
		perror("rwpf: DIOCXBEGIN");
		return 1;
	}
	struct pfioc_pooladdr pp;
	struct pfioc_rule pr;

	/*
	 * scrub on EXT inet all max-mss 1400: CLAT46 makes a 1500-byte IPv4 packet a 1520-byte IPv6 one, more than the
	 * cellular link takes; TCP segments are kept small from the SYN on (both ways), so pages do not stall until path
	 * MTU discovery catches up (a client's first page took long, 2026-11-19).
	 */
	if (pool_begin(&pp) < 0) {
		perror("rwpf: DIOCBEGINADDRS (scrub)");
		return 1;
	}
	memset(&pr, 0, sizeof(pr));
	pr.action = PF_CHANGE_NONE;
	pr.ticket = ticket[0];
	pr.pool_ticket = pp.ticket;
	pr.rule.action = PF_SCRUB;
	pr.rule.af = AF_INET;
	strlcpy(pr.rule.ifname, argv[2], sizeof(pr.rule.ifname));
	pr.rule.src.addr.type = PF_ADDR_ADDRMASK;
	pr.rule.dst.addr.type = PF_ADDR_ADDRMASK;
	pr.rule.max_mss = 1400;
	if (ioctl(dev, DIOCADDRULE, &pr) < 0) {
		perror("rwpf: DIOCADDRULE (scrub)");
		return 1;
	}

	/* the translation address: a one-address pool */
	if (pool_begin(&pp) < 0) {
		perror("rwpf: DIOCBEGINADDRS");
		return 1;
	}
	pp.af = AF_INET;
	pp.addr.addr.type = PF_ADDR_ADDRMASK;
	pp.addr.addr.v.a.addr.v4addr = addr;
	memset(&pp.addr.addr.v.a.mask.v4addr, 0xff, 4);
	if (ioctl(dev, DIOCADDADDR, &pp) < 0) {
		perror("rwpf: DIOCADDADDR");
		return 1;
	}
	memset(&pr, 0, sizeof(pr));
	pr.action = PF_CHANGE_NONE;
	pr.ticket = ticket[1];
	pr.pool_ticket = pp.ticket;
	pr.rule.action = PF_NAT;
	pr.rule.af = AF_INET;
	pr.rule.direction = PF_OUT;
	strlcpy(pr.rule.ifname, argv[2], sizeof(pr.rule.ifname));
	pr.rule.src.addr.type = PF_ADDR_ADDRMASK;
	pr.rule.src.addr.v.a.addr.v4addr = net;
	pr.rule.src.addr.v.a.mask.v4addr.s_addr = len ? htonl(0xffffffffu << (32 - len)) : 0;
	pr.rule.dst.addr.type = PF_ADDR_ADDRMASK;             /* any */
	pr.rule.rpool.proxy_port[0] = 50001;          /* pfctl's PF_NAT_PROXY_PORT_LOW (not in pfvar.h) */
	pr.rule.rpool.proxy_port[1] = 65535;
	if (ioctl(dev, DIOCADDRULE, &pr) < 0) {
		perror("rwpf: DIOCADDRULE");
		return 1;
	}
	if (xact(DIOCXCOMMIT, ticket)) {
		perror("rwpf: DIOCXCOMMIT");
		return 1;
	}
	if (ioctl(dev, DIOCSTART) < 0 && errno != EEXIST) {
		perror("rwpf: DIOCSTART");
		return 1;
	}
	forwarding(1);
	printf("rwpf: scrub on %s max-mss 1400; nat on %s inet from %s/%d to any -> %s; pf on, forwarding on\n", argv[2], argv[2],
	    nb, len, argv[3]);
	return 0;
}
