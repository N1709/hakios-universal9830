/*
 * teetsd: TEEGRIS trusted storage for iOS (A, 2026-11-14) -- the file half of Samsung's tzdaemon_ts.c.
 * The kernel (bsd/dev/redwood_teegris.c) holds the session with the secure world's storage service and hands
 * each request over kern.redwood.tee_ts: a 0x41000-byte buffer, u32 group (5), u32 TS_CMD_*, u32 length,
 * u32 status (written here), payload at +0x10. This answers it and writes the buffer back.
 *
 * Storage lives under ROOT (default /private/var/root/tee-ts): Samsung's /mnt/vendor/efs/tee/srt (storage id
 * 0x80000000) is ROOT/efs/srt, /data/vendor/tee/srt is ROOT/data/srt. The phone's EFS partition is never used.
 *
 * Status codes (tzdaemon): 1 ok, 4 not found, 6 bad request, 8 error, 11 storage missing.
 * Commands 1..12: WRITE_DATA OVERWRITE_DATA READ_DATA TRUNCATE_DATA READ_OBJ_LIST DELETE_OBJECT DELETE_STORAGE
 * WRITE_OBJ_LIST CHECK_STORAGE MARK_TO_DEL RESTORE_STORAGE FINI_DEL. Unimplemented ones are logged with a dump.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#define BUF_SIZE        0x41000
#define EFS_ID          0x80000000u

enum { ST_OK = 1, ST_NOT_FOUND = 4, ST_BAD = 6, ST_ERROR = 8, ST_NO_STORAGE = 11 };
static const char *const names[13] = { "?", "WRITE_DATA", "OVERWRITE_DATA", "READ_DATA", "TRUNCATE_DATA",
	"READ_OBJ_LIST", "DELETE_OBJECT", "DELETE_STORAGE", "WRITE_OBJ_LIST", "CHECK_STORAGE", "MARK_TO_DEL",
	"RESTORE_STORAGE", "FINI_DEL" };
static const char *root = "/private/var/root/tee-ts";
static FILE *logf;

static void
say(const char *fmt, ...)
{
	va_list ap;
	time_t t = time(NULL);
	char ts[32];

	strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
	fprintf(logf, "%s ", ts);
	va_start(ap, fmt);
	vfprintf(logf, fmt, ap);
	va_end(ap);
	fflush(logf);
}

static void
dump(const uint8_t *b, size_t n)
{
	for (size_t i = 0; i < n; i += 16) {
		fprintf(logf, "    %04zx:", i);
		for (size_t j = i; j < i + 16 && j < n; j++) {
			fprintf(logf, " %02x", b[j]);
		}
		fprintf(logf, "\n");
	}
	fflush(logf);
}

static void
uuid_str(const uint8_t *u, char out[37])
{
	snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", u[0], u[1], u[2], u[3],
	    u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

/* Samsung's <base>/<uuid>/<id %02u>, under ROOT */
static void
storage_path(const uint8_t *uuid, uint32_t id, char *out, size_t n)
{
	char u[37];

	uuid_str(uuid, u);
	snprintf(out, n, "%s/%s/srt/%s/%02u", root, id == EFS_ID ? "efs" : "data", u, id);
}

static void
mkdirs(const char *path)
{
	char p[1024];

	strlcpy(p, path, sizeof(p));
	for (char *s = p + 1; *s; s++) {
		if (*s == '/') {
			*s = 0;
			mkdir(p, 0700);
			*s = '/';
		}
	}
	mkdir(p, 0700);
}

static uint32_t
check_storage(uint8_t *payload)
{
	char path[1024];
	uint32_t id;
	struct stat st;

	memcpy(&id, payload + 16, 4);
	storage_path(payload, id, path, sizeof(path));
	if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
		say("  storage %s: exists\n", path);
		return ST_OK;
	}
	say("  storage %s: %s\n", path, errno == ENOENT ? "none yet" : strerror(errno));
	return errno == ENOENT ? ST_NOT_FOUND : ST_ERROR;
}

int
main(int argc, char **argv)
{
	static uint8_t buf[BUF_SIZE];

	if (argc > 1) {
		root = argv[1];
	}
	logf = fopen("/private/var/root/teetsd.log", "a");
	if (logf == NULL) {
		logf = stderr;
	}
	mkdirs(root);
	{
		char p[1024];

		snprintf(p, sizeof(p), "%s/efs/srt", root);
		mkdirs(p);
		snprintf(p, sizeof(p), "%s/data/srt", root);
		mkdirs(p);
	}
	say("teetsd: storage under %s\n", root);
	for (;;) {
		size_t n = sizeof(buf);
		uint32_t group, cmd, len, status;

		if (sysctlbyname("kern.redwood.tee_ts", buf, &n, NULL, 0) != 0) {
			say("read: %s\n", strerror(errno));
			sleep(1);
			continue;
		}
		if (n == 0) {
			continue;               /* (no request in 500 ms) */
		}
		memcpy(&group, buf, 4);
		memcpy(&cmd, buf + 4, 4);
		memcpy(&len, buf + 8, 4);
		say("request group %u cmd %u %s len %u\n", group, cmd, cmd <= 12 ? names[cmd] : "?", len);
		if (group != 5 || cmd < 1 || cmd > 12) {
			status = ST_BAD;
		} else if (cmd == 9) {
			status = check_storage(buf + 16);
		} else {
			say("  not implemented; payload:\n");
			dump(buf, 0x80);
			status = ST_ERROR;
		}
		memcpy(buf + 12, &status, 4);
		say("  -> status %u\n", status);
		if (sysctlbyname("kern.redwood.tee_ts", NULL, NULL, buf, sizeof(buf)) != 0) {
			say("answer: %s\n", strerror(errno));
		}
	}
}
