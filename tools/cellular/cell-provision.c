/*
 * MIT. cell-provision: the modem service's files in /var/root, made from the phone's OWN firmware (2026-11-20).
 *
 * A clean data volume had none of them, so redwood-cellulard ended at once ("no modem", no SIM) although everything
 * it needs is on the phone. Nothing Samsung's is shipped or committed: this reads the phone's partitions, READ-ONLY
 * (opened O_RDONLY, never written; EFS and the modem partition stay untouched):
 *
 *   /dev/rufs_radio  -> cell-radio.img (the whole partition), and from its table of contents (the first 800 bytes,
 *                       32-byte entries {name[12], offset, load, size, crc, id}):
 *                       cell-toc.bin (TOC), cell-boot.bin (BOOT), cell-boot.info ("S512" 1 size crc load id),
 *                       cell-plan.txt (MAIN, APM, VSS from the radio image; NV from cell-nv.bin; "15" last)
 *   /dev/rufs_efs    -> cell-nv.bin = nv_data.bin, the modem's own settings, read out of the EFS ext4 file system
 *
 * Only files that are missing are made (an existing cell-nv.bin, which the modem updates, is kept). Each is written
 * to a temporary name and renamed. Exit 0 when every file is there afterwards.
 *   cell-provision            make what is missing
 *   cell-provision --check    say what is missing, make nothing
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DIR_ "/var/root/"         /* where redwood-cellulard reads them (cell-plan.txt names these paths) */
static const char *out_dir = DIR_;    /* CELL_PROVISION_DIR=... for a test run beside the real files */
#define RADIO_DEV "/dev/rufs_radio"
#define EFS_DEV "/dev/rufs_efs"
#define BLK 4096                        /* the raw UFS devices want 4 KB aligned reads */

static int check_only;

/* a read of any offset/length from a raw device that only takes 4 KB aligned reads */
static int
dev_read(int fd, uint64_t off, void *out, size_t len)
{
	uint64_t a = off & ~(uint64_t)(BLK - 1), end = (off + len + BLK - 1) & ~(uint64_t)(BLK - 1);
	size_t n = (size_t)(end - a);
	uint8_t *buf;
	ssize_t got;

	if (posix_memalign((void **)&buf, BLK, n)) {
		return -1;
	}
	got = pread(fd, buf, n, (off_t)a);
	if (got < (ssize_t)(off - a + len)) {
		free(buf);
		return -1;
	}
	memcpy(out, buf + (off - a), len);
	free(buf);
	return 0;
}

static int
exists(const char *name)
{
	char p[256];
	struct stat st;

	snprintf(p, sizeof(p), "%s%s", out_dir, name);
	return stat(p, &st) == 0 && st.st_size > 0;
}

/* write a whole file through a temporary name; mode 0600 */
static int
put(const char *name, const void *data, size_t len)
{
	char p[256], t[272];
	int fd;

	snprintf(p, sizeof(p), "%s%s", out_dir, name);
	snprintf(t, sizeof(t), "%s.provision", p);
	fd = open(t, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		return -1;
	}
	for (size_t done = 0; done < len;) {
		ssize_t w = write(fd, (const uint8_t *)data + done, len - done);
		if (w <= 0) {
			close(fd);
			unlink(t);
			return -1;
		}
		done += (size_t)w;
	}
	if (fsync(fd) || close(fd) || rename(t, p)) {
		unlink(t);
		return -1;
	}
	printf("cell-provision: %s (%zu bytes)\n", name, len);
	return 0;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* ---------------------------------------------------------------- the modem partition */
struct toc_entry { char name[13]; uint32_t off, load, size, crc, id; };

static int
toc_find(const uint8_t *toc, const char *name, struct toc_entry *e)
{
	for (unsigned i = 0; i < 800; i += 32) {
		if (strncmp((const char *)toc + i, name, 12) == 0) {
			memcpy(e->name, toc + i, 12);
			e->name[12] = 0;
			e->off = le32(toc + i + 12);
			e->load = le32(toc + i + 16);
			e->size = le32(toc + i + 20);
			e->crc = le32(toc + i + 24);
			e->id = le32(toc + i + 28);
			return 0;
		}
	}
	return -1;
}

static int
provision_radio(void)
{
	static const char *want[] = { "cell-radio.img", "cell-toc.bin", "cell-boot.bin", "cell-boot.info", "cell-plan.txt" };
	struct toc_entry boot, main_, apm, vss, nv;
	uint8_t toc[800];
	int missing = 0, fd, rc = 0;

	for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		if (!exists(want[i])) {
			printf("cell-provision: missing %s\n", want[i]);
			missing = 1;
		}
	}
	if (!missing || check_only) {
		return missing;
	}
	fd = open(RADIO_DEV, O_RDONLY);
	if (fd < 0 || dev_read(fd, 0, toc, sizeof(toc)) || strncmp((char *)toc, "TOC", 3) != 0) {
		printf("cell-provision: %s: no readable table of contents (%s)\n", RADIO_DEV, strerror(errno));
		if (fd >= 0) close(fd);
		return 1;
	}
	if (toc_find(toc, "BOOT", &boot) || toc_find(toc, "MAIN", &main_) || toc_find(toc, "APM", &apm) ||
	    toc_find(toc, "VSS", &vss) || toc_find(toc, "NV", &nv) || boot.size == 0 || boot.size > 131072) {
		printf("cell-provision: the radio TOC lacks BOOT/MAIN/APM/VSS/NV\n");
		close(fd);
		return 1;
	}
	if (!exists("cell-radio.img")) {    /* the whole partition, as the old by-hand copy was */
		/* up to the end of the last section the TOC names (INFO sits past VSS), rounded to 4 KB: all the modem
		 * service reads (cell-plan.txt offsets). The by-hand copy was the whole 70 MB partition; the rest is unused. */
		struct toc_entry info;
		uint64_t size = (uint64_t)vss.off + vss.size;
		uint8_t *img;

		if (toc_find(toc, "INFO", &info) == 0 && (uint64_t)info.off + info.size > size) size = (uint64_t)info.off + info.size;
		size = (size + BLK - 1) & ~(uint64_t)(BLK - 1);
		img = malloc(size);
		if (img == NULL || dev_read(fd, 0, img, size) || put("cell-radio.img", img, size)) rc = 1;
		free(img);
	}
	if (!exists("cell-toc.bin") && put("cell-toc.bin", toc, sizeof(toc))) rc = 1;
	if (!exists("cell-boot.bin")) {
		uint8_t b[131072];
		if (dev_read(fd, boot.off, b, boot.size) || put("cell-boot.bin", b, boot.size)) rc = 1;
	}
	if (!exists("cell-boot.info")) {    /* "S512", 1, size, crc, load, id (redwood-cellulard checks each) */
		uint8_t info[24];
		uint32_t f[5] = { 1, boot.size, boot.crc, boot.load, boot.id };
		memcpy(info, "S512", 4);
		memcpy(info + 4, f, sizeof(f));
		if (put("cell-boot.info", info, sizeof(info))) rc = 1;
	}
	if (!exists("cell-plan.txt")) {     /* the download plan: stage id, file, offset, size, crc */
		char plan[512];
		int n = snprintf(plan, sizeof(plan), "%u " DIR_ "cell-radio.img %u %u %08x\n%u " DIR_ "cell-radio.img %u %u 0\n"
		    "%u " DIR_ "cell-radio.img %u %u 0\n%u " DIR_ "cell-nv.bin 0 %u 0\n15\n", main_.id, main_.off, main_.size,
		    main_.crc, apm.id, apm.off, apm.size, vss.id, vss.off, vss.size, nv.id, nv.size);
		if (put("cell-plan.txt", plan, (size_t)n)) rc = 1;
	}
	close(fd);
	return rc;
}

/* ---------------------------------------------------------------- EFS: nv_data.bin out of ext4, read-only */
struct ext4 { int fd; uint32_t bs, ipg, isz, dsz, first; int big; };

static int
ext4_open_dev(struct ext4 *x, const char *dev)
{
	uint8_t sb[1024];

	x->fd = open(dev, O_RDONLY);
	if (x->fd < 0 || dev_read(x->fd, 1024, sb, sizeof(sb)) || le16(sb + 56) != 0xef53) {
		return -1;
	}
	x->bs = 1024u << le32(sb + 24);
	x->ipg = le32(sb + 40);
	x->isz = le16(sb + 88);
	x->big = (le32(sb + 96) & 0x80) != 0;           /* INCOMPAT_64BIT */
	x->dsz = x->big ? le16(sb + 254) : 32;
	x->first = le32(sb + 20);                       /* s_first_data_block */
	return x->bs >= 1024 && x->bs <= 65536 && x->ipg && x->isz >= 128 ? 0 : -1;
}

static int
ext4_open(struct ext4 *x)
{
	return ext4_open_dev(x, EFS_DEV);
}

static int
ext4_inode(struct ext4 *x, uint32_t n, uint8_t *ino)
{
	uint32_t g = (n - 1) / x->ipg, i = (n - 1) % x->ipg;
	uint8_t d[64];
	uint64_t table;

	if (dev_read(x->fd, (uint64_t)(x->first + 1) * x->bs + (uint64_t)g * x->dsz, d, x->dsz > 64 ? 64 : x->dsz)) {
		return -1;
	}
	table = le32(d + 8) | (x->big ? (uint64_t)le32(d + 0x28) << 32 : 0);
	return dev_read(x->fd, table * x->bs + (uint64_t)i * x->isz, ino, x->isz > 256 ? 256 : x->isz);
}

/* the file's bytes [0, len) through its extent tree (depth up to 4) */
static int
ext4_extents(struct ext4 *x, const uint8_t *node, uint8_t *out, uint64_t len, int depth_left)
{
	uint16_t entries = le16(node + 2), depth = le16(node + 6);

	if (le16(node) != 0xf30a || depth_left < 0) {
		return -1;
	}
	for (unsigned k = 0; k < entries; k++) {
		const uint8_t *e = node + 12 + 12 * k;
		if (depth == 0) {
			uint64_t lblk = le32(e), start = le32(e + 8) | (uint64_t)le16(e + 6) << 32;
			uint32_t n = le16(e + 4);
			if (n > 32768) n -= 32768;      /* an unwritten extent: zeros (already) */
			for (uint32_t b = 0; b < n; b++) {
				uint64_t at = (lblk + b) * x->bs;
				if (at >= len) break;
				uint64_t c = len - at < x->bs ? len - at : x->bs;
				if (dev_read(x->fd, (start + b) * x->bs, out + at, (size_t)c)) return -1;
			}
		} else {
			uint8_t *blk = malloc(x->bs);
			uint64_t leaf = le32(e + 4) | (uint64_t)le16(e + 8) << 32;
			int r = blk == NULL || dev_read(x->fd, leaf * x->bs, blk, x->bs) ||
			    ext4_extents(x, blk, out, len, depth_left - 1);
			free(blk);
			if (r) return -1;
		}
	}
	return 0;
}

static uint8_t *
ext4_file(struct ext4 *x, uint32_t n, uint64_t *len)
{
	uint8_t ino[256], *data;

	if (ext4_inode(x, n, ino) || !(le32(ino + 32) & 0x80000)) {    /* EXT4_EXTENTS_FL */
		return NULL;
	}
	*len = le32(ino + 4) | (uint64_t)le32(ino + 108) << 32;
	if (*len == 0 || *len > (64u << 20)) {
		return NULL;
	}
	data = calloc(1, (size_t)*len);
	if (data == NULL || ext4_extents(x, ino + 40, data, *len, 4)) {
		free(data);
		return NULL;
	}
	return data;
}

static uint32_t
ext4_lookup(struct ext4 *x, uint32_t dir, const char *name)
{
	uint64_t len;
	uint8_t *d = ext4_file(x, dir, &len);
	uint32_t found = 0;
	size_t nl = strlen(name);

	for (uint64_t at = 0; d && at + 8 <= len;) {
		uint32_t inode = le32(d + at);
		uint16_t rec = le16(d + at + 4);
		uint8_t l = d[at + 6];
		if (rec < 8 || at + rec > len) break;
		if (inode && l == nl && memcmp(d + at + 8, name, nl) == 0) {
			found = inode;
			break;
		}
		at += rec;
	}
	free(d);
	return found;
}

static int
provision_nv(void)
{
	struct ext4 x = { -1 };
	uint32_t n;
	uint64_t len;
	uint8_t *nv;
	int rc;

	if (exists("cell-nv.bin")) {
		return 0;
	}
	printf("cell-provision: missing cell-nv.bin\n");
	if (check_only) {
		return 1;
	}
	if (ext4_open(&x)) {
		printf("cell-provision: %s: not a readable ext4\n", EFS_DEV);
		if (x.fd >= 0) close(x.fd);
		return 1;
	}
	n = ext4_lookup(&x, 2, "nv_data.bin");
	nv = n ? ext4_file(&x, n, &len) : NULL;
	close(x.fd);
	if (nv == NULL || len != 1048576) {
		printf("cell-provision: no 1 MB nv_data.bin in EFS (inode %u)\n", n);
		free(nv);
		return 1;
	}
	rc = put("cell-nv.bin", nv, (size_t)len);
	free(nv);
	return rc;
}

/* ------------------------------------------------- --fp: the fingerprint sensor's factory calibration (2026-11-20)
 * Samsung keeps it in /efs/biometrics/meta (cbge_0.dat, cbge_8.dat: the background slots; sensor_id). Without it a
 * clean install's fingerprint images fail the TA's quality checks (redwood_teegris.c "fp cbgeefs" loads the slots
 * from /var/root/fp-calib). Read-only, from whichever EFS partition holds it. */
static int
provision_fp(void)
{
	static const char *devs[] = { EFS_DEV, "/dev/rufs_sec_efs" };
	static const char *files[] = { "cbge_0.dat", "cbge_8.dat", "sensor_id" };
	int need = 0;

	for (unsigned i = 0; i < 3; i++) need |= !exists(files[i]);
	if (!need) {
		printf("cell-provision --fp: calibration present\n");
		return 0;
	}
	if (check_only) return 1;
	for (unsigned d = 0; d < 2; d++) {
		struct ext4 x = { -1 };
		uint32_t bio, meta;
		int rc = 0, got = 0;

		if (ext4_open_dev(&x, devs[d])) {
			if (x.fd >= 0) close(x.fd);
			continue;
		}
		bio = ext4_lookup(&x, 2, "biometrics");
		meta = bio ? ext4_lookup(&x, bio, "meta") : 0;
		for (unsigned i = 0; meta && i < 3; i++) {
			uint64_t len;
			uint32_t n = exists(files[i]) ? 0 : ext4_lookup(&x, meta, files[i]);
			uint8_t *data = n ? ext4_file(&x, n, &len) : NULL;
			if (data) {
				rc |= put(files[i], data, (size_t)len);
				got++;
				printf("cell-provision --fp: %s (%llu bytes) from %s\n", files[i], (unsigned long long)len, devs[d]);
				free(data);
			}
		}
		close(x.fd);
		if (meta) return rc || got == 0;
	}
	printf("cell-provision --fp: no biometrics/meta in EFS\n");
	return 1;
}

int
main(int argc, char **argv)
{
	int r;

	if (argc > 1 && strcmp(argv[1], "--fp") == 0) {     /* the fingerprint calibration instead of the modem files */
		out_dir = getenv("CELL_PROVISION_DIR") ? getenv("CELL_PROVISION_DIR") : "/var/root/fp-calib/";
		mkdir(out_dir, 0700);
		check_only = argc > 2 && strcmp(argv[2], "--check") == 0;
		return provision_fp() ? 1 : 0;
	}
	check_only = argc > 1 && strcmp(argv[1], "--check") == 0;
	if (getenv("CELL_PROVISION_DIR")) out_dir = getenv("CELL_PROVISION_DIR");
	r = provision_radio();
	r |= provision_nv();
	if (!r) printf("cell-provision: all modem files present\n");
	return r ? 1 : 0;
}
