/*
 * en1: Ethernet over the USB cable (x1slte, SM-G980F, 2026-11-06), the network side of the CDC-ECM function the USB
 * device link offers next to its serial port (redwood_dwc3.c with usb_ecm=1; the descriptors in redwood_usb_acm.h).
 * The computer's own driver (macOS, Linux: CDC-ECM, nothing to install) sees an Ethernet adapter.
 *
 * Self-contained, so as not to touch en0 (the radio's, redwood_net.c): the interface is made with the kernel's interface
 * KPI as redwood_net.c makes its own; frames on their way out wait in a small queue that the USB poll empties
 * (redwood_usbnet_tx_take), frames that came in are handed up from the poll's thread (redwood_usbnet_rx).
 *
 * The addresses, so that nothing has to be set up on the computer:
 *   the phone   10.77.0.2/24, set here (SIOCPROTOATTACH, SIOCAIFADDR through a socket of the kernel's own)
 *   the computer 10.77.0.1, which it is given by DHCP: a small server here answers DISCOVER with OFFER and REQUEST with
 *              ACK (no router, no DNS: the computer's own network stays its own), from inside the frame path
 * Then from the Mac: ssh root@10.77.0.2, ping 10.77.0.2.
 * Ethernet addresses, locally administered: 02:52:57:00:00:02 the phone, 02:52:57:00:00:01 the computer's end.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/kalloc.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_ether.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/kpi_interface.h>
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <pexpert/pexpert.h>
#include <sys/kern_event.h>
#include <sys/kpi_mbuf.h>
#include <sys/kpi_socket.h>
#include <sys/sockio.h>
#include <sys/systm.h>

#define FRAME_MAX               1518u
#define TXQ                     64u                     /* frames waiting for the cable: a power of two */
#define PHONE_IP                0x0a4d0002u             /* 10.77.0.2 */
#define HOST_IP                 0x0a4d0001u             /* 10.77.0.1 */
#define NETMASK                 0xffffff00u

static const uint8_t phone_mac[6] = { 0x02, 0x52, 0x57, 0x00, 0x00, 0x02 };

static LCK_GRP_DECLARE(usbnet_grp, "redwood-usbnet");
static LCK_SPIN_DECLARE(usbnet_lock, &usbnet_grp);
static ifnet_t usbnet_ifp;
static bool usbnet_up, usbnet_started;
static struct { uint16_t len; uint8_t data[FRAME_MAX]; } *txq;
static uint32_t tx_head, tx_tail;
static struct { uint64_t sent, sent_bytes, received, received_bytes, dropped_out, dropped_in, dhcp; } count;

/* ---- the queue to the cable ---- */

static bool
txq_put(const uint8_t *frame, size_t len, mbuf_t m)
{
	bool ok = false;

	lck_spin_lock(&usbnet_lock);
	if (txq != NULL && tx_head - tx_tail < TXQ && len <= FRAME_MAX) {
		uint32_t slot = tx_head & (TXQ - 1);

		if (m != NULL) {
			ok = mbuf_copydata(m, 0, len, txq[slot].data) == 0;
		} else {
			memcpy(txq[slot].data, frame, len);
			ok = true;
		}
		if (ok) {
			txq[slot].len = (uint16_t)len;
			tx_head++;
		}
	}
	lck_spin_unlock(&usbnet_lock);
	return ok;
}

/* the next frame for the cable into buf (at most cap bytes): its length, 0 if none */
size_t redwood_usbnet_tx_take(uint8_t *buf, size_t cap);

size_t
redwood_usbnet_tx_take(uint8_t *buf, size_t cap)
{
	size_t len = 0;

	lck_spin_lock(&usbnet_lock);
	if (tx_head != tx_tail) {
		uint32_t slot = tx_tail & (TXQ - 1);

		len = txq[slot].len <= cap ? txq[slot].len : 0;
		if (len) {
			memcpy(buf, txq[slot].data, len);
		}
		tx_tail++;
	}
	lck_spin_unlock(&usbnet_lock);
	return len;
}

/* ---- the DHCP server: the computer's address ---- */

static uint16_t
ip_sum(const uint8_t *p, size_t n)
{
	uint32_t s = 0;

	for (size_t i = 0; i + 1 < n; i += 2) {
		s += (uint32_t)(p[i] << 8 | p[i + 1]);
	}
	while (s >> 16) {
		s = (s & 0xffff) + (s >> 16);
	}
	return (uint16_t)~s;
}

static void
put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

/* a BOOTP/DHCP request from the computer (UDP 68 -> 67): true if it was one (and so is not handed up) */
static bool
dhcp_answer(const uint8_t *f, size_t len)
{
	const uint8_t *ip = f + 14, *udp, *bootp, *opt, *end = f + len;
	uint8_t kind = 0, out[14 + 20 + 8 + 300];
	uint8_t *oi = out + 14, *ou = oi + 20, *ob = ou + 8, *o;

	if (len < 14 + 20 + 8 + 240 || f[12] != 0x08 || f[13] != 0x00 || ip[9] != 17) {
		return false;
	}
	udp = ip + 4 * (ip[0] & 0xf);
	if (udp + 8 + 240 > end || (udp[0] << 8 | udp[1]) != 68 || (udp[2] << 8 | udp[3]) != 67) {
		return false;
	}
	bootp = udp + 8;
	if (bootp[0] != 1 || bootp[236] != 99 || bootp[237] != 130 || bootp[238] != 83 || bootp[239] != 99) {
		return true;
	}
	for (opt = bootp + 240; opt < end && *opt != 255;) {
		if (*opt == 0) {
			opt++;
			continue;
		}
		if (opt + 2 > end || opt + 2 + opt[1] > end) {
			break;
		}
		if (opt[0] == 53 && opt[1] == 1) {
			kind = opt[2];
		}
		opt += 2 + opt[1];
	}
	if (kind != 1 && kind != 3) {           /* DISCOVER, REQUEST; RELEASE, INFORM and the rest: nothing to say */
		return true;
	}
	memset(out, 0, sizeof(out));
	memset(out, 0xff, 6);                   /* to everyone: the computer has no address yet */
	memcpy(out + 6, phone_mac, 6);
	out[12] = 0x08;
	ob[0] = 2;                              /* BOOTREPLY */
	ob[1] = 1;
	ob[2] = 6;
	memcpy(ob + 4, bootp + 4, 4);           /* xid */
	memcpy(ob + 10, bootp + 10, 2);         /* flags */
	put32(ob + 16, HOST_IP);                /* yiaddr */
	put32(ob + 20, PHONE_IP);               /* siaddr */
	memcpy(ob + 28, bootp + 28, 16);        /* chaddr */
	ob[236] = 99; ob[237] = 130; ob[238] = 83; ob[239] = 99;
	o = ob + 240;
	*o++ = 53; *o++ = 1; *o++ = kind == 1 ? 2 : 5;          /* OFFER, ACK */
	*o++ = 54; *o++ = 4; put32(o, PHONE_IP); o += 4;          /* server */
	*o++ = 51; *o++ = 4; put32(o, 86400); o += 4;             /* a day */
	*o++ = 1; *o++ = 4; put32(o, NETMASK); o += 4;
	*o++ = 255;
	/* UDP 67 -> 68, no checksum (allowed on IPv4); IPv4 from the phone to everyone */
	ou[0] = 0; ou[1] = 67; ou[2] = 0; ou[3] = 68;
	ou[4] = (uint8_t)((8 + 300) >> 8); ou[5] = (uint8_t)(8 + 300);
	oi[0] = 0x45;
	oi[2] = (uint8_t)((20 + 8 + 300) >> 8); oi[3] = (uint8_t)(20 + 8 + 300);
	oi[8] = 64;
	oi[9] = 17;
	put32(oi + 12, PHONE_IP);
	put32(oi + 16, 0xffffffffu);
	{
		uint16_t sum = ip_sum(oi, 20);

		oi[10] = (uint8_t)(sum >> 8);
		oi[11] = (uint8_t)sum;
	}
	if (txq_put(out, sizeof(out), NULL)) {
		count.dhcp++;
		printf("usbnet: DHCP %s to the computer (%02x:%02x:%02x:%02x:%02x:%02x): 10.77.0.1\n", kind == 1 ? "OFFER" : "ACK",
		    bootp[28], bootp[29], bootp[30], bootp[31], bootp[32], bootp[33]);
	}
	return true;
}

/* ---- frames up from the cable ---- */

void redwood_usbnet_rx(const uint8_t *frame, size_t length);

void
redwood_usbnet_rx(const uint8_t *frame, size_t length)
{
	struct ifnet_stat_increment_param stats = { .packets_in = 1, .bytes_in = (uint32_t)length };
	unsigned int chunks = 1;
	mbuf_t packet = NULL;
	char *header;

	if (usbnet_ifp == NULL || length < ETHER_HDR_LEN || length > FRAME_MAX) {
		count.dropped_in++;
		return;
	}
	if (dhcp_answer(frame, length)) {
		return;
	}
	if (mbuf_allocpacket(MBUF_DONTWAIT, length, &chunks, &packet) != 0) {
		count.dropped_in++;
		return;
	}
	if (mbuf_copyback(packet, 0, length, frame, MBUF_DONTWAIT) != 0) {
		mbuf_freem(packet);
		count.dropped_in++;
		return;
	}
	header = mbuf_data(packet);
	mbuf_pkthdr_setrcvif(packet, usbnet_ifp);
	mbuf_pkthdr_setheader(packet, header);
	mbuf_pkthdr_adjustlen(packet, -ETHER_HDR_LEN);
	mbuf_setdata(packet, header + ETHER_HDR_LEN, length - ETHER_HDR_LEN);
	bpf_tap_in(usbnet_ifp, DLT_EN10MB, packet, header, ETHER_HDR_LEN);
	count.received++;
	count.received_bytes += length;
	ifnet_input(usbnet_ifp, packet, &stats);
}

/* ---- the interface ---- */

static errno_t
usbnet_output(ifnet_t ifp, mbuf_t packet)
{
	size_t length = mbuf_pkthdr_len(packet);

	if (usbnet_up && length <= FRAME_MAX) {
		bpf_tap_out(ifp, DLT_EN10MB, packet, NULL, 0);
	}
	if (usbnet_up && txq_put(NULL, length, packet)) {
		count.sent++;
		count.sent_bytes += length;
		ifnet_stat_increment_out(ifp, 1, (uint32_t)length, 0);
	} else {
		count.dropped_out++;
		ifnet_stat_increment_out(ifp, 0, 0, 1);
	}
	mbuf_freem(packet);
	return 0;
}

static errno_t
usbnet_ioctl(ifnet_t ifp, u_long command, void *data)
{
	const int media[] = { IFM_ETHER | IFM_AUTO };
	struct ifreq *request = data;
	struct ifmediareq *media_request = data;
	user_addr_t list;
	errno_t error = 0;

	switch (command) {
	case SIOCSIFADDR:
		ifnet_set_flags(ifp, IFF_UP, IFF_UP);
		break;
	case SIOCSIFFLAGS:
		ifnet_set_flags(ifp, (ifnet_flags(ifp) & IFF_UP) ? IFF_RUNNING : 0, IFF_RUNNING);
		break;
	case SIOCGIFMEDIA32:
	case SIOCGIFMEDIA64:
		list = command == SIOCGIFMEDIA64 ? ((struct ifmediareq64 *)media_request)->ifmu_ulist :
		    CAST_USER_ADDR_T(((struct ifmediareq32 *)media_request)->ifmu_ulist);
		media_request->ifm_active = media_request->ifm_current = media[0];
		media_request->ifm_mask = 0;
		media_request->ifm_status = IFM_AVALID | (usbnet_up ? IFM_ACTIVE : 0);
		if (list != USER_ADDR_NULL && media_request->ifm_count > 0) {
			error = copyout(media, list, sizeof(media));
		}
		media_request->ifm_count = 1;
		break;
	case SIOCGIFDEVMTU:
		request->ifr_devmtu.ifdm_current = ifnet_mtu(ifp);
		request->ifr_devmtu.ifdm_max = ETHERMTU;
		request->ifr_devmtu.ifdm_min = IF_MINMTU;
		break;
	case SIOCSIFMTU:
		error = request->ifr_mtu > ETHERMTU || request->ifr_mtu < IF_MINMTU ? EINVAL : ifnet_set_mtu(ifp, request->ifr_mtu);
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		break;
	default:
		error = EOPNOTSUPP;
		break;
	}
	return error;
}

static void
usbnet_event(uint32_t code)
{
	_Alignas(struct kern_event_msg) char message[sizeof(struct kern_event_msg) + sizeof(struct net_event_data)] = { 0 };
	struct kern_event_msg *header = (struct kern_event_msg *)message;
	struct net_event_data *about = (struct net_event_data *)(header + 1);

	header->total_size = sizeof(message);
	header->vendor_code = KEV_VENDOR_APPLE;
	header->kev_class = KEV_NETWORK_CLASS;
	header->kev_subclass = KEV_DL_SUBCLASS;
	header->event_code = code;
	about->if_family = ifnet_family(usbnet_ifp);
	about->if_unit = ifnet_unit(usbnet_ifp);
	strlcpy(about->if_name, ifnet_name(usbnet_ifp), IFNAMSIZ);
	ifnet_event(usbnet_ifp, header);
}

/* the phone's address on en1, as ifconfig would set it */
static void
usbnet_address(void)
{
	struct ifaliasreq ifra;
	struct ifreq ifr;
	struct sockaddr_in *sin;
	socket_t so = NULL;
	errno_t e1, e2;

	if (sock_socket(PF_INET, SOCK_DGRAM, 0, NULL, NULL, &so) != 0) {
		printf("usbnet: no socket to set the address with\n");
		return;
	}
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s%u", ifnet_name(usbnet_ifp), ifnet_unit(usbnet_ifp));
	e1 = sock_ioctl(so, SIOCPROTOATTACH, &ifr);
	memset(&ifra, 0, sizeof(ifra));
	strlcpy(ifra.ifra_name, ifr.ifr_name, sizeof(ifra.ifra_name));
	sin = (struct sockaddr_in *)&ifra.ifra_addr;
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(PHONE_IP);
	sin = (struct sockaddr_in *)&ifra.ifra_mask;
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(NETMASK);
	sin = (struct sockaddr_in *)&ifra.ifra_broadaddr;
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = htonl(PHONE_IP | ~NETMASK);
	e2 = sock_ioctl(so, SIOCAIFADDR, &ifra);
	sock_close(so);
	printf("usbnet: %s is 10.77.0.2/24 (attach %d, address %d)\n", ifr.ifr_name, e1, e2);
}

static void
usbnet_thread(__unused void *p, __unused wait_result_t w)
{
	struct ifnet_init_eparams init = {
		.ver = IFNET_INIT_CURRENT_VERSION,
		.len = sizeof(init),
		.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO,
		.uniqueid = "redwood-usb-en",
		.uniqueid_len = 14,
		.name = "en",
		.unit = 1,
		.family = IFNET_FAMILY_ETHERNET,
		.subfamily = IFNET_SUBFAMILY_USB,
		.type = IFT_ETHER,
		.output = usbnet_output,
		.demux = ether_demux,
		.add_proto = ether_add_proto,
		.del_proto = ether_del_proto,
		.check_multi = ether_check_multi,
		.framer_extended = ether_frameout_extended,
		.ioctl = usbnet_ioctl,
		.broadcast_addr = etherbroadcastaddr,
		.broadcast_len = ETHER_ADDR_LEN,
	};
	ifnet_t ifp = NULL;
	errno_t error;
	uint64_t said = 0;

	txq = kalloc_data(sizeof(*txq) * TXQ, Z_WAITOK | Z_ZERO);
	error = txq == NULL ? ENOMEM : ifnet_allocate_extended(&init, &ifp);
	if (error == 0) {
		ifnet_set_capabilities_enabled(ifp, 0, -1);
		ifnet_set_addrlen(ifp, ETHER_ADDR_LEN);
		ifnet_set_baudrate(ifp, 480ULL * 1000 * 1000);
		ifnet_set_mtu(ifp, ETHERMTU);
		ifnet_set_flags(ifp, IFF_BROADCAST | IFF_MULTICAST | IFF_SIMPLEX, 0xffff);
		ifnet_set_hdrlen(ifp, ETHER_HDR_LEN);
		ifnet_set_offload(ifp, 0);
		error = ifnet_attach(ifp, NULL);
	}
	if (error != 0) {
		printf("usbnet: the interface cannot be made: error %d\n", error);
		return;
	}
	ifnet_set_lladdr(ifp, phone_mac, ETHER_ADDR_LEN);
	bpfattach(ifp, DLT_EN10MB, ETHER_HDR_LEN);
	usbnet_ifp = ifp;
	ifnet_set_flags(ifp, IFF_UP | IFF_RUNNING, IFF_UP | IFF_RUNNING);
	usbnet_address();
	if (usbnet_up) {
		usbnet_event(KEV_DL_LINK_ON);
	}
	printf("usbnet: en%u attached (Ethernet over USB, 02:52:57:00:00:02)\n", ifnet_unit(ifp));
	for (;;) {
		uint64_t total = count.sent + count.received;

		delay_for_interval(60, NSEC_PER_SEC);
		if (total != said) {
			said = total;
			printf("usbnet: %llu frames sent (%llu bytes), %llu received (%llu bytes); dropped %llu out, %llu in; %llu DHCP "
			    "answers\n", count.sent, count.sent_bytes, count.received, count.received_bytes, count.dropped_out,
			    count.dropped_in, count.dhcp);
		}
	}
}

/* the USB link: the host has the network function's frames on (alternate 1) or off */
void redwood_usbnet_link(bool up);

void
redwood_usbnet_link(bool up)
{
	thread_t thread;

	if (!usbnet_started) {
		usbnet_started = true;
		if (kernel_thread_start(usbnet_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
	if (up == usbnet_up) {
		return;
	}
	usbnet_up = up;
	printf("usbnet: the computer has %s the network function\n", up ? "switched on" : "switched off");
	if (!up) {
		lck_spin_lock(&usbnet_lock);
		tx_tail = tx_head;
		lck_spin_unlock(&usbnet_lock);
	}
	if (usbnet_ifp != NULL) {
		usbnet_event(up ? KEV_DL_LINK_ON : KEV_DL_LINK_OFF);
	}
}
