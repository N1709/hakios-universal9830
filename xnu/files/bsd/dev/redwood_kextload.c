/*
 * Apple's lifs.kext, loaded by this kernel at run time (A, 2026-11-20; docs/devices/x1slte/lifs-loader.md).
 *
 * Files shows external drives through userfsd and livefileproviderd, and livefileproviderd needs lifs (IOKit
 * com_apple_filesystems_lifs, user client AppleLIFSUserClient). This kernel has no kext loader; lifs needs very little:
 * an arm64 MH_KEXT_BUNDLE whose relocations are all 64-bit absolute pointers (ARM64_RELOC_UNSIGNED): local ones
 * (+= the load address) and external ones (= one of our kernel's symbols + addend, from redwood_kextsyms.c). Code
 * reaches imports through its GOT, so no instruction is patched.
 *
 * Boot-arg lifs=1 (default off). A kernel thread waits for the system volume, reads
 * /System/Library/Extensions/lifs.kext/lifs, checks every load command and relocation, maps, relocates, makes its text
 * read-only + executable (redwood_pmap_kernel_text), gives its typed-allocation views zones, registers its classes
 * under the kernel's own OSKext, starts it (kmod_info.start) and hands IOKit its personality. kern.redwood.lifs: state.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <sys/sysctl.h>
#include <kern/thread.h>
#include <kern/kalloc.h>
#include <vm/vm_kern.h>
#include <mach/kmod.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/reloc.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>

#define LIFS_PATH               "/System/Library/Extensions/lifs.kext/lifs"
#define LIFS_MAX_FILE           (1024 * 1024)

struct redwood_kextsym { const char *name; const void *addr; };
extern const struct redwood_kextsym redwood_kextsyms[];
extern const size_t redwood_kextsyms_count;

extern int redwood_pmap_kernel_text(vm_offset_t va, vm_size_t size, int upto, uint64_t *out_pte);
static int lifs_text_upto;      /* kern.redwood.lifs_text: 1..4 do only the first steps of making text executable */
static uint64_t lifs_text_pte;
extern unsigned redwood_kalloc_type_adopt(vm_offset_t sect, vm_size_t bytes, unsigned *count);
typedef void (*redwood_ctor_t)(void);
extern int redwood_kext_classes(redwood_ctor_t *ctors, unsigned count);
extern bool redwood_kext_lifs_personality(void);

static char lifs_state[200] = "off (boot-arg lifs=1: on request, 2: at boot)";
SYSCTL_DECL(_kern_redwood);
SYSCTL_STRING(_kern_redwood, OID_AUTO, lifs, CTLFLAG_RD | CTLFLAG_LOCKED, lifs_state, 0, "Apple's lifs.kext, loaded at run time");

static int lifs_stop, lifs_step;       /* kern.redwood.lifs_stop: stop after step N (1..6), to find a hang */
#define STEP(...) do { lifs_step++; printf("lifs: step %d: ", lifs_step); printf(__VA_ARGS__); printf("\n"); \
	if (lifs_stop != 0 && lifs_step > lifs_stop) { snprintf(lifs_state, sizeof(lifs_state), "stopped before step %d", lifs_step); kept = 1; goto out; } \
	IOSleep(50); } while (0)
#define FAIL(...) do { snprintf(lifs_state, sizeof(lifs_state), __VA_ARGS__); printf("lifs: %s\n", lifs_state); goto out; } while (0)

static const void *
kernel_symbol(const char *name)
{
	size_t lo = 0, hi = redwood_kextsyms_count;

	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		int c = strcmp(name, redwood_kextsyms[mid].name);

		if (c == 0) {
			return redwood_kextsyms[mid].addr;
		}
		if (c < 0) {
			hi = mid;
		} else {
			lo = mid + 1;
		}
	}
	return NULL;
}

/* the whole file into kernel memory: 0 or an errno */
static int
read_file(const char *path, uint8_t **out, size_t *outlen)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	off_t size = 0;
	int resid = 0, error;
	uint8_t *buf;

	error = vnode_open(path, FREAD, 0, 0, &vp, ctx);
	if (error != 0) {
		return error;
	}
	error = vnode_size(vp, &size, ctx);
	if (error == 0 && (size <= 0 || size > LIFS_MAX_FILE)) {
		error = EFBIG;
	}
	if (error == 0) {
		buf = kalloc_data((vm_size_t)size, Z_WAITOK | Z_ZERO);
		if (buf == NULL) {
			error = ENOMEM;
		} else {
			error = vn_rdwr(UIO_READ, vp, (caddr_t)buf, (int)size, 0, UIO_SYSSPACE, 0, vfs_context_ucred(ctx), &resid,
			    vfs_context_proc(ctx));
			if (error == 0 && resid != 0) {
				error = EIO;
			}
			if (error != 0) {
				kfree_data(buf, (vm_size_t)size);
			} else {
				*out = buf;
				*outlen = (size_t)size;
			}
		}
	}
	vnode_close(vp, FREAD, ctx);
	return error;
}

static void
lifs_load(void)
{
	uint8_t *file = NULL;
	size_t flen = 0;
	const struct mach_header_64 *mh;
	const struct symtab_command *st = NULL;
	const struct dysymtab_command *dy = NULL;
	uint64_t vmend = 0, text_lo = 0, text_hi = 0, kmod_off = 0;
	uint64_t ctor_off = 0, ctor_size = 0, kt_off = 0, kt_size = 0;
	vm_offset_t base = 0;
	vm_size_t vmsize = 0;
	kmod_info_t *ki;
	int error, r, kept = 0;
	const uint8_t *lc, *lc_end;

	lifs_step = 0;

	STEP("read %s", LIFS_PATH);
	error = read_file(LIFS_PATH, &file, &flen);
	if (error != 0) {
		FAIL("cannot read %s: %d", LIFS_PATH, error);
	}
	mh = (const struct mach_header_64 *)file;
	if (flen < sizeof(*mh) || mh->magic != MH_MAGIC_64 || mh->cputype != CPU_TYPE_ARM64 || mh->filetype != MH_KEXT_BUNDLE ||
	    sizeof(*mh) + mh->sizeofcmds > flen) {
		FAIL("not an arm64 kext bundle");
	}

	/* pass 1: load commands; segments must lie inside the file and below 4 MB of address space */
	lc = file + sizeof(*mh);
	lc_end = lc + mh->sizeofcmds;
	for (uint32_t i = 0; i < mh->ncmds; i++) {
		const struct load_command *l = (const struct load_command *)lc;

		if (lc + sizeof(*l) > lc_end || l->cmdsize < sizeof(*l) || lc + l->cmdsize > lc_end) {
			FAIL("bad load command %u", i);
		}
		if (l->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *s = (const struct segment_command_64 *)lc;
			const struct section_64 *sect = (const struct section_64 *)(s + 1);

			if (s->fileoff + s->filesize > flen || s->vmaddr + s->vmsize > 4 * 1024 * 1024 || s->filesize > s->vmsize ||
			    sizeof(*s) + s->nsects * sizeof(*sect) > l->cmdsize) {
				FAIL("bad segment %.16s", s->segname);
			}
			if (s->vmaddr + s->vmsize > vmend) {
				vmend = s->vmaddr + s->vmsize;
			}
			if (strncmp(s->segname, "__TEXT_EXEC", 16) == 0) {
				text_lo = s->vmaddr;
				text_hi = s->vmaddr + s->vmsize;
			}
			for (uint32_t k = 0; k < s->nsects; k++) {
				if (strncmp(sect[k].sectname, "__mod_init_func", 16) == 0) {
					ctor_off = sect[k].addr;
					ctor_size = sect[k].size;
				} else if (strncmp(sect[k].sectname, "__kalloc_type", 16) == 0) {
					kt_off = sect[k].addr;
					kt_size = sect[k].size;
				} else if (strncmp(sect[k].sectname, "__kalloc_var", 16) == 0 && sect[k].size != 0) {
					FAIL("variable-size kalloc views: not handled");
				}
			}
		} else if (l->cmd == LC_SYMTAB) {
			st = (const struct symtab_command *)lc;
		} else if (l->cmd == LC_DYSYMTAB) {
			dy = (const struct dysymtab_command *)lc;
		}
		lc += l->cmdsize;
	}
	if (st == NULL || dy == NULL || text_hi == 0 || (text_lo & PAGE_MASK) || (text_hi & PAGE_MASK) ||
	    st->symoff + (uint64_t)st->nsyms * sizeof(struct nlist_64) > flen || st->stroff + (uint64_t)st->strsize > flen ||
	    dy->extreloff + (uint64_t)dy->nextrel * sizeof(struct relocation_info) > flen ||
	    dy->locreloff + (uint64_t)dy->nlocrel * sizeof(struct relocation_info) > flen ||
	    ctor_off + ctor_size > vmend || kt_off + kt_size > vmend) {
		FAIL("symbol table, relocations or sections out of the file");
	}

	STEP("checked %zu bytes; mapping %llu bytes", flen, vmend);
	/* pass 2: memory for every segment, wired, zeroed; the file's bytes at their addresses */
	vmsize = round_page(vmend);
	if (kmem_alloc(kernel_map, &base, vmsize, KMA_ZERO | KMA_KOBJECT | KMA_PERMANENT, VM_KERN_MEMORY_KEXT) != KERN_SUCCESS) {
		base = 0;
		FAIL("no memory (%llu bytes)", (uint64_t)vmsize);
	}
	lc = file + sizeof(*mh);
	for (uint32_t i = 0; i < mh->ncmds; i++) {
		const struct load_command *l = (const struct load_command *)lc;

		if (l->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *s = (const struct segment_command_64 *)lc;

			memcpy((void *)(base + s->vmaddr), file + s->fileoff, s->filesize);
		}
		lc += l->cmdsize;
	}

	STEP("mapped at %p; relocating", (void *)base);
	/* pass 3: relocations; only 64-bit absolute pointers inside the image */
	{
		const struct relocation_info *rel = (const struct relocation_info *)(file + dy->locreloff);
		const struct nlist_64 *syms = (const struct nlist_64 *)(file + st->symoff);
		const char *strs = (const char *)(file + st->stroff);

		for (uint32_t i = 0; i < dy->nlocrel; i++) {
			if (rel[i].r_type != 0 || rel[i].r_length != 3 || rel[i].r_pcrel || rel[i].r_extern || rel[i].r_address < 0 ||
			    (uint64_t)rel[i].r_address + 8 > vmend) {
				FAIL("local relocation %u not a pointer", i);
			}
			*(uint64_t *)(base + (uint32_t)rel[i].r_address) += base;
		}
		rel = (const struct relocation_info *)(file + dy->extreloff);
		for (uint32_t i = 0; i < dy->nextrel; i++) {
			const char *name;
			const void *addr;

			if (rel[i].r_type != 0 || rel[i].r_length != 3 || rel[i].r_pcrel || !rel[i].r_extern || rel[i].r_address < 0 ||
			    (uint64_t)rel[i].r_address + 8 > vmend || rel[i].r_symbolnum >= st->nsyms ||
			    syms[rel[i].r_symbolnum].n_un.n_strx >= st->strsize) {
				FAIL("external relocation %u malformed", i);
			}
			name = strs + syms[rel[i].r_symbolnum].n_un.n_strx;
			if (strnlen(name, st->strsize - syms[rel[i].r_symbolnum].n_un.n_strx) ==
			    st->strsize - syms[rel[i].r_symbolnum].n_un.n_strx) {
				FAIL("external relocation %u: unterminated name", i);
			}
			addr = kernel_symbol(name);
			if (addr == NULL) {
				FAIL("unresolved %s", name);
			}
			*(uint64_t *)(base + (uint32_t)rel[i].r_address) += (uint64_t)addr;
		}
		/* kmod_info: a defined symbol */
		for (uint32_t i = 0; i < st->nsyms; i++) {
			if ((syms[i].n_type & N_TYPE) == N_SECT && syms[i].n_un.n_strx < st->strsize &&
			    strncmp(strs + syms[i].n_un.n_strx, "_kmod_info", st->strsize - syms[i].n_un.n_strx) == 0) {
				kmod_off = syms[i].n_value;
			}
		}
		if (kmod_off == 0 || kmod_off + sizeof(kmod_info_t) > vmend) {
			FAIL("no kmod_info");
		}
	}

	/* pass 4: text read-only and executable; typed views placed; classes; start; IOKit */
	STEP("relocated; text %#llx-%#llx executable", text_lo, text_hi);
	if (redwood_pmap_kernel_text(base + text_lo, text_hi - text_lo, lifs_text_upto, &lifs_text_pte) != 0) {
		FAIL("no page entry for the text (pte %#llx)", lifs_text_pte);
	}
	STEP("text done (sub-steps %d, first pte was %#llx); typed views", lifs_text_upto, lifs_text_pte);
	{
		unsigned nviews = 0;

		r = (int)redwood_kalloc_type_adopt(base + kt_off, kt_size, &nviews);
		printf("lifs: %llu KB at %p, text %#llx-%#llx, %u+%u relocations, %d of %u typed views placed\n",
		    (uint64_t)vmsize / 1024, (void *)base, text_lo, text_hi, dy->nlocrel, dy->nextrel, r, nviews);
	}
	STEP("classes (%llu constructors)", ctor_size / sizeof(void *));
	error = redwood_kext_classes((redwood_ctor_t *)(base + ctor_off), (unsigned)(ctor_size / sizeof(void *)));
	if (error != 0) {
		FAIL("class registration: %#x", error);
	}
	ki = (kmod_info_t *)(base + kmod_off);
	ki->address = base;
	ki->size = vmsize;
	ki->hdr_size = 0;
	STEP("start %s %s", ki->name, ki->version);
	if (ki->start != NULL) {
		error = ki->start(ki, NULL);
		if (error != KERN_SUCCESS) {
			FAIL("start: %d", error);
		}
	}
	STEP("started; personality to IOKit");
	if (!redwood_kext_lifs_personality()) {
		FAIL("IOKit did not take the personality");
	}
	snprintf(lifs_state, sizeof(lifs_state), "loaded: %s %s at %p (%llu KB), started, personality given", ki->name,
	    ki->version, (void *)base, (uint64_t)vmsize / 1024);
	printf("lifs: %s\n", lifs_state);
	base = 0;               /* it stays */
out:
	if (base != 0 && lifs_step <= 3) {
		/* nothing of it ran or became executable yet: give the memory back */
		kmem_free(kernel_map, base, vmsize);
	}
	(void)kept;
	if (file != NULL) {
		kfree_data(file, flen);
	}
}

static void
lifs_thread(__unused void *arg, __unused wait_result_t wr)
{
	/* the system volume is mounted a few seconds into the boot; wait for the file, up to two minutes */
	for (int i = 0; i < 120; i++) {
		vnode_t vp = NULLVP;

		if (rootvnode != NULLVP && vnode_lookup(LIFS_PATH, 0, &vp, vfs_context_kernel()) == 0) {
			vnode_put(vp);
			lifs_load();
			return;
		}
		IOSleep(1000);
	}
	snprintf(lifs_state, sizeof(lifs_state), "%s never appeared", LIFS_PATH);
}

/* lifs=1: loaded when asked (kern.redwood.lifs_load=1, once); lifs=2: also at boot, from a thread */
static uint32_t lifs_mode;
static int lifs_loaded;

static int
sysctl_lifs_load SYSCTL_HANDLER_ARGS
{
	int v = lifs_loaded, error = sysctl_handle_int(oidp, &v, 0, req);

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (lifs_mode == 0) {
		return EPERM;
	}
	if (v == 1 && !lifs_loaded) {
		lifs_loaded = 1;
		lifs_load();
		if (strncmp(lifs_state, "stopped", 7) == 0) {
			lifs_loaded = 0;        /* a staged run: may run again */
		}
	}
	return 0;
}
SYSCTL_INT(_kern_redwood, OID_AUTO, lifs_text, CTLFLAG_RW | CTLFLAG_LOCKED, &lifs_text_upto, 0, "making text executable: only steps 1..N (0 all)");
SYSCTL_QUAD(_kern_redwood, OID_AUTO, lifs_text_pte, CTLFLAG_RD | CTLFLAG_LOCKED, &lifs_text_pte, "the first text page's entry, before");
SYSCTL_INT(_kern_redwood, OID_AUTO, lifs_stop, CTLFLAG_RW | CTLFLAG_LOCKED, &lifs_stop, 0, "stop the load after step N (1..6)");
SYSCTL_INT(_kern_redwood, OID_AUTO, lifs_step, CTLFLAG_RD | CTLFLAG_LOCKED, &lifs_step, 0, "the last step begun");
SYSCTL_PROC(_kern_redwood, OID_AUTO, lifs_load, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_lifs_load, "I",
    "1: load Apple's lifs.kext now (boot-arg lifs=1 or 2)");

void redwood_kextload_init(void);
void
redwood_kextload_init(void)
{
	thread_t t;

	/* default 2 (A, 2026-11-21: loads, starts and runs userfsd's stack): at boot; lifs=1 on request, lifs=0 off */
	if (!PE_parse_boot_argn("lifs", &lifs_mode, sizeof(lifs_mode))) {
		lifs_mode = 2;
	}
	if (lifs_mode == 0) {
		return;
	}
	if (lifs_mode == 1) {
		snprintf(lifs_state, sizeof(lifs_state), "ready: sysctl kern.redwood.lifs_load=1 loads it");
		return;
	}
	snprintf(lifs_state, sizeof(lifs_state), "waiting for %s", LIFS_PATH);
	lifs_loaded = 1;
	if (kernel_thread_start(lifs_thread, NULL, &t) == KERN_SUCCESS) {
		thread_deallocate(t);
	}
}
