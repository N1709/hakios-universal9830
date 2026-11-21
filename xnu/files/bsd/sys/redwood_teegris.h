/*
 * TEEGRIS client inside the kernel (bsd/dev/redwood_teegris.c): open a trusted application's session with its image,
 * invoke its commands. Wire structures are Samsung tzdev's (tz_common.h).
 */
#ifndef _SYS_REDWOOD_TEEGRIS_H_
#define _SYS_REDWOOD_TEEGRIS_H_

#include <stdint.h>
#include <stdbool.h>

struct tz_uuid {
	uint32_t time_low;
	uint16_t time_mid;
	uint16_t time_hi_and_version;
	uint8_t clock_seq_and_node[8];
} __attribute__((packed));

/* the client credential the secure world sees with shared memory and socket messages */
struct tz_cred {
	uint32_t pid;
	uint32_t uid;
	uint32_t gid;
	uint8_t hash[32];
} __attribute__((packed));

/* parameter types: the GlobalPlatform TEEC values (TEEC_MEMREF_TEMP_* for buffers) */
#define REDWOOD_TEE_NONE                0
#define REDWOOD_TEE_VALUE_INPUT         1
#define REDWOOD_TEE_VALUE_OUTPUT        2
#define REDWOOD_TEE_VALUE_INOUT         3
#define REDWOOD_TEE_MEMREF_INPUT        5
#define REDWOOD_TEE_MEMREF_OUTPUT       6
#define REDWOOD_TEE_MEMREF_INOUT        7

struct redwood_tee_param {
	uint32_t type;
	uint32_t a, b;          /* values */
	void *buf;              /* buffers: kernel memory, copied through shared memory */
	uint32_t size;          /* in: its size; out: what the TA returned */
	int shm_id;             /* (internal) */
};

struct redwood_tee_session;

bool redwood_tee_up(void);
void redwood_tee_set_client(const struct tz_cred *cred);
int redwood_tee_open(const char *ta_path, const struct tz_uuid *uuid, struct redwood_tee_session **out,
    uint32_t *result, uint32_t *origin);
int redwood_tee_invoke(struct redwood_tee_session *session, uint32_t cmd_id, struct redwood_tee_param params[4],
    uint32_t *result, uint32_t *origin);
void redwood_tee_close(struct redwood_tee_session *session);

#endif /* _SYS_REDWOOD_TEEGRIS_H_ */
