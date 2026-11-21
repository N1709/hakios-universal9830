#include "../../xnu/files/bsd/dev/redwood_usb_acm.h"
#include "../../xnu/files/bsd/dev/redwood_usbmux_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
	uint8_t version[20] = {0, 0, 0, 0, 0, 0, 0, 20, 0, 0, 0, 2};
	for (size_t n = 0; n < sizeof(version); n++)
		assert(rwmux_frame(version, n, 0) == 0);
	assert(rwmux_frame(version, sizeof(version), 0) == 20);
	uint8_t packet[64] = {0};
	rwmux_put32(packet, 6);
	rwmux_put32(packet + 4, 36);
	rwmux_put32(packet + 8, RWMUX_MAGIC);
	for (size_t n = 0; n < 36; n++)
		assert(rwmux_frame(packet, n, 2) == 0);
	assert(rwmux_frame(packet, 36, 2) == 36);
	assert(rwmux_frame(packet, 64, 2) == 36);
	packet[8] ^= 1;
	assert(rwmux_frame(packet, 36, 2) == -1);
	packet[8] ^= 1;
	rwmux_put32(packet + 4, 15);
	assert(rwmux_frame(packet, 36, 2) == -1);
	rwmux_put32(packet + 4, RWMUX_MAX + 1);
	assert(rwmux_frame(packet, 36, 2) == -1);
	for (unsigned speed = 0; speed < 2; speed++) {
		uint8_t config[256];
		size_t n = usbacm_config_desc_mux(config, speed, 2);
		assert(n == 177 && config[2] == 177 && config[4] == 5);
		unsigned interfaces = 0, epmask = 0;
		for (size_t at = 0; at < n;) {
			assert(config[at] >= 2 && at + config[at] <= n);
			if (config[at + 1] == 4 && config[at + 3] == 0)
				interfaces++;
			if (config[at + 1] == 5) {
				uint8_t ep = config[at + 2];
				if (ep == 5)
					epmask |= 1;
				if (ep == 0x85)
					epmask |= 2;
			}
			at += config[at];
		}
		assert(interfaces == 5 && epmask == 3);
	}
        /* Hosts may send a 48 KiB mux frame in three 16 KiB USB transfers. */
        static uint8_t large[49152];
        rwmux_put32(large,6);rwmux_put32(large+4,sizeof(large));rwmux_put32(large+8,RWMUX_MAGIC);
        assert(rwmux_frame(large,16384,2)==0);
        assert(rwmux_frame(large,32768,2)==0);
        assert(rwmux_frame(large,sizeof(large),2)==sizeof(large));
	struct usbacm s;
	struct usbacm_reply reply;
	usbacm_init(&s);
	s.ecm = true;
	s.mux = true;
	uint8_t getdev[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};
	assert(usbacm_setup(&s, getdev, &reply) == USBACM_IN);
	assert(reply.len == 18 && reply.data[8] == 0xac && reply.data[9] == 5);
	s.mux = false;
	assert(usbacm_setup(&s, getdev, &reply) == USBACM_IN);
	assert(reply.data[8] == 9 && reply.data[9] == 0x12);
	puts("USB mux framing and composite descriptors: PASS");
	return 0;
}
