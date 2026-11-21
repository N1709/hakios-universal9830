/*
 * The debug port (A, 2026-11-06): one sysctl, kern.redwood.poke, that takes a command and answers it, so a register
 * can be read or tried on the running phone instead of a rebuild and a reboot. From the Mac: tools/poke 'r 0x18c51000 4'
 * (ssh s20 rwsys kern.redwood.poke '...'). Root only (sysctl writes need root).
 *
 * Built in:
 *   r PA [N]          N 32-bit words (at most 64) at physical PA       w PA V        one word, read back
 *   m PA AND OR       read-modify-write: (old & AND) | OR
 *   sr PA / sw PA V   the same through EL3 (Samsung's SMC_CMD_REG -101: SFRs the secure world owns, e.g. ABOX's GIC)
 *   help              every command, the drivers' too
 * Drivers add commands with redwood_dbgport_add() (sys/redwood_dbgport.h), e.g. the sound driver's snd.send.
 * Physical pages are mapped once and kept (64 at most, 16 KB each). Nothing here runs unless a command is written.
 */
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/redwood_dbgport.h>
#include <sys/sysctl.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <kern/locks.h>
#include <sys/redwood_spi8_debug.h>
#include <sys/redwood_cp_memory.h>
#include <sys/redwood_cp_ipc.h>
#include <vm/memory_types.h>
#include <pexpert/device_tree.h>
#include <machine/machine_routines.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t size);

#define SMC_CMD_REG     0xFFFFFFFFFFFFFF9BULL

static char out_buf[8192];
static struct { uint64_t page; volatile uint8_t *va; } maps[64];
static unsigned nmaps;
#ifdef GENERIC_GIC_V2_IRQ
static vm_offset_t cp_ipc_va; /* fixed retained IPC, protected by dbgport_mtx */
static vm_offset_t cp_pp_va;  /* pktproc window (CP_IPC_PP_FLAG), Normal-NC, protected by dbgport_mtx */
#endif
static struct { char name[32]; const char *help; redwood_dbgport_fn fn; } cmds[96];
static unsigned ncmds;
static LCK_GRP_DECLARE(dbgport_grp, "redwood_dbgport");
static LCK_MTX_DECLARE(dbgport_mtx, &dbgport_grp);

int
redwood_dbgport_add(const char *name, const char *help, redwood_dbgport_fn fn)
{
	if (ncmds >= sizeof(cmds) / sizeof(cmds[0]) || name == NULL || fn == NULL) {
		return -1;
	}
	strlcpy(cmds[ncmds].name, name, sizeof(cmds[ncmds].name));
	cmds[ncmds].help = help;
	cmds[ncmds].fn = fn;
	ncmds++;
	return 0;
}

static volatile uint8_t *
pmap_pa(uint64_t pa)
{
	uint64_t page = pa & ~0x3fffULL;
#ifdef GENERIC_GIC_V2_IRQ
    if (cp_ipc_va && page>=CP_IPC_BASE && page<CP_IPC_BASE+CP_IPC_SIZE) return NULL;
    if (cp_pp_va && page>=CP_IPC_PP_BASE && page<CP_IPC_PP_BASE+CP_IPC_PP_SPAN) return NULL;
#endif

	for (unsigned i = 0; i < nmaps; i++) {
		if (maps[i].page == page) {
			return maps[i].va + (pa - page);
		}
	}
	if (nmaps >= sizeof(maps) / sizeof(maps[0])) {
		return NULL;
	}
	maps[nmaps].va = (volatile uint8_t *)ml_io_map((vm_offset_t)page, 0x4000);
	if (maps[nmaps].va == NULL) {
		return NULL;
	}
	maps[nmaps].page = page;
	return maps[nmaps++].va + (pa - page);
}

#ifdef GENERIC_GIC_V2_IRQ
static int
cp_memory_certified(void)
{
    DTEntry chosen;void *property;unsigned length;uint64_t values[3];
    const char *names[]={"cellular-memory-version","cellular-ipc-base","cellular-ipc-size"};
    if (SecureDTLookupEntry(NULL,"/chosen",&chosen)!=kSuccess) return 0;
    for (unsigned i=0;i<3;i++) {
        if (SecureDTGetProperty(chosen,names[i],&property,&length)!=kSuccess || length!=8) return 0;
        memcpy(&values[i],property,8);
    }
    return cp_memory_certificate(values[0],values[1],values[2]);
}

static uint64_t
cp_memory_translation(vm_offset_t va)
{
    uint64_t saved,par;
    boolean_t interrupts=ml_set_interrupts_enabled(FALSE);
    __asm__ volatile("mrs %0, par_el1\n\tat s1e1r, %2\n\tisb\n\tmrs %1, par_el1\n\tmsr par_el1, %0\n\tisb"
        : "=&r"(saved),"=&r"(par) : "r"(va) : "memory");
    ml_set_interrupts_enabled(interrupts);
    return par;
}


/* cp.s2mpu: Samsung's hypervisor lets the modem's PCIe block (HSI2, S2MPU instance 25, VID 0) read and
 * write the two CP shared-memory regions, as s5100_poweron_pcie() does with exynos_set_dev_stage2_ap(
 * "hsi2", 0, base, size, ATTR_RW) for every CP shmem region but MSI. Only with the loader's certificate;
 * only these fixed regions; the hypervisor's return per region is reported. */
#define CP_HVC_SET_S2MPU 0xc6000100ULL
#define CP_S2MPU_HSI2    ((0ULL << 16) | 25ULL)
#define CP_S2MPU_RW      3ULL
static uint64_t
cp_hvc5(uint64_t function,uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4)
{
    register uint64_t x0 __asm__("x0")=function;
    register uint64_t x1 __asm__("x1")=a1;
    register uint64_t x2 __asm__("x2")=a2;
    register uint64_t x3 __asm__("x3")=a3;
    register uint64_t x4 __asm__("x4")=a4;
    __asm__ volatile("dsb sy\n\thvc #0\n\tdsb sy" : "+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3),"+r"(x4) : :
        "x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","memory");
    return x0;
}
static void
cp_s2mpu_grant(void)
{
    /* + the voice-call path (A, 2026-11-13): ABOX's PCI region (abox_pci.c exynos_set_dev_stage2_ap("hsi2", 0,
     * abox-pci-rmem 0xF4200000, 8 MB)) and the VTS mailbox page (dtb s2mpu_mailbox 0x155a0000, 4 KB) */
    static const uint64_t regions[][2]={{0xf0000000ULL,0x800000ULL},{0xd0000000ULL,0x4000000ULL},
        {0xf4200000ULL,0x800000ULL},{0x155a0000ULL,0x1000ULL}};
    uint64_t ret[4];
    if (!cp_memory_certified()) {
        snprintf(out_buf,sizeof(out_buf),"cp.s2mpu: missing/invalid loader reservation certificate; nothing granted\n");return;
    }
    for (unsigned i=0;i<4;i++) ret[i]=cp_hvc5(CP_HVC_SET_S2MPU,CP_S2MPU_HSI2,regions[i][0],regions[i][1],CP_S2MPU_RW);
    snprintf(out_buf,sizeof(out_buf),"cp.s2mpu: HSI2 RW f0000000+800000 ret %#llx, d0000000+4000000 ret %#llx, "
        "abox pci f4200000+800000 ret %#llx, vts mailbox 155a0000+1000 ret %#llx\n",
        (unsigned long long)ret[0],(unsigned long long)ret[1],(unsigned long long)ret[2],(unsigned long long)ret[3]);
}

/* Audit the existing direct-map alias first. No RAM read/write, new mapping,
 * HVC, CP power or DMA operation. A cached alias requires resolution before
 * a NormalNC mapping is allowed. The debug mutex serializes commands. */
static void
cp_memory_audit(void)
{
    if (!cp_memory_certified()) goto missing;
    /* phystokv panics for a PA outside the kernel's physical aperture. */
    extern unsigned long gPhysBase,real_phys_size;
    if (CP_IPC_BASE<gPhysBase || CP_IPC_BASE-gPhysBase>real_phys_size ||
        CP_IPC_SIZE>real_phys_size-(CP_IPC_BASE-gPhysBase)) {
        snprintf(out_buf,sizeof(out_buf),"cp.memory: IPC outside physical aperture; no translation attempted\n");return;
    }
    unsigned nc=0,cached_or_device=0,fault=0,wrong_pa=0,debug_alias=0;
    for (unsigned i=0;i<nmaps;i++)
        if (maps[i].page>=CP_IPC_BASE && maps[i].page<CP_IPC_BASE+CP_IPC_SIZE) debug_alias++;
    for (uint64_t pa=CP_IPC_BASE;pa<CP_IPC_BASE+CP_IPC_SIZE;pa+=0x1000) {
        uint64_t par=cp_memory_translation(ml_static_ptovirt((vm_offset_t)pa));
        if (par&1) fault++;
        else if (!cp_memory_par_matches(par,pa)) wrong_pa++;
        else if (cp_memory_par_nc(par,pa)) nc++;
        else cached_or_device++;
    }
    snprintf(out_buf,sizeof(out_buf),"cp.memory: certificate1 pages2048 nc%u other_cache%u unmapped%u wrong_pa%u debug_alias%u; audit only, no DMA\n",
        nc,cached_or_device,fault,wrong_pa,debug_alias);
    return;
missing:
    snprintf(out_buf,sizeof(out_buf),"cp.memory: missing/invalid loader reservation certificate; no access\n");
}
#endif

static uint64_t
smc(uint64_t f, uint64_t a1, uint64_t a2, uint64_t *x2out)
{
	register uint64_t x0 __asm__("x0") = f;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = 0;

	__asm__ volatile ("dsb sy\n\tsmc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
            "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	if (x2out != NULL) {
		*x2out = x2;
	}
	return x0;
}

#ifdef GENERIC_GIC_V2_IRQ
/* tzdev's version ABI has seven input registers; zero every unused argument.
 * Keep this independent of the four-register Samsung SFR debug helper. */
static uint64_t
fp_tee_smc(uint64_t function, uint64_t client, uint64_t driver, uint64_t *out_x2)
{
    register uint64_t x0 __asm__("x0") = function;
    register uint64_t x1 __asm__("x1") = client;
    register uint64_t x2 __asm__("x2") = driver;
    register uint64_t x3 __asm__("x3") = 0;
    register uint64_t x4 __asm__("x4") = 0;
    register uint64_t x5 __asm__("x5") = 0;
    register uint64_t x6 __asm__("x6") = 0;
    __asm__ volatile ("dsb sy\n\tsmc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3),
        "+r"(x4), "+r"(x5), "+r"(x6) : : "x7", "x8", "x9", "x10", "x11", "x12", "x13",
        "x14", "x15", "x16", "x17", "memory");
    *out_x2=x2;
    return x0;
}
#include <sys/redwood_fp_probe.h>
#include <kern/thread.h>
#include <vm/vm_kern.h>
extern uint64_t kvtophys(vm_offset_t);
extern processor_t cpu_to_processor(int);
extern processor_t thread_bind(processor_t);
extern wait_result_t thread_block(thread_continue_t);
extern unsigned int real_ncpus;
extern int psci_smp_cpu_is_running(int);
static vm_offset_t fp_channel_pages[9];
static unsigned fp_channel_count;
static unsigned fp_channel_cpu_count(void) { return real_ncpus; }
static bool fp_channel_online(unsigned cpu) {
    return cpu_to_processor((int)cpu)!=PROCESSOR_NULL && psci_smp_cpu_is_running((int)cpu);
}
static void fp_channel_flush(vm_offset_t page) {
    for(vm_offset_t p=page;p<page+PAGE_SIZE;p+=64) __asm__ volatile ("dc civac, %0" : : "r"(p) : "memory");
    __asm__ volatile ("dsb sy" : : : "memory");
}
static bool fp_channel_allocate(unsigned index,uint32_t *pfn) {
    vm_offset_t page=0;
    if(index>=9 || PAGE_SIZE<4096 || kmem_alloc(kernel_map,&page,PAGE_SIZE,
        KMA_KOBJECT|KMA_ZERO|KMA_DATA,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS)return false;
    /* Retain even a failed translation: at most nine private pages per boot. */
    fp_channel_pages[index]=page;fp_channel_count=index+1;
    uint64_t pa=kvtophys(page);
    if(!pa || (pa&4095) || (pa>>12)>UINT32_MAX)return false;
    fp_channel_flush(page);*pfn=(uint32_t)(pa>>12);return true;
}
static uint64_t fp_channel_call(unsigned cpu,uint64_t function,uint64_t arg1,uint64_t arg2) {
    processor_t previous=thread_bind(cpu_to_processor((int)cpu));
    thread_block(THREAD_CONTINUE_NULL);
    uint64_t unused=0,result=fp_tee_smc(function,arg1,arg2,&unused);
    thread_bind(previous);thread_block(THREAD_CONTINUE_NULL);return result;
}
static void fp_channel_service_prepare(uint32_t pfn) {
    memcpy((void *)fp_channel_pages[0],&pfn,sizeof(pfn));
    fp_channel_flush(fp_channel_pages[0]);
    fp_channel_flush(fp_channel_pages[fp_channel_count-1]);
}
static void fp_channel_service_read(uint32_t *cpu,uint32_t *user) {
    vm_offset_t page=fp_channel_pages[fp_channel_count-1];fp_channel_flush(page);
    memcpy(cpu,(void *)page,4);memcpy(user,(void *)(page+4),4);
}
#include <sys/redwood_fp_channels.h>
/* Actual saved base DT /teegris: SPI 222/223, level-high => GIC INTID 254/255.
 * An ISR masks on its first event: no interrupt storm before a scheduler exists.
 * Never acknowledge a panic as a successful operation or start work in the ISR. */
extern kern_return_t gic_v3_spi_attach_to(uint32_t,bool,void (*)(void *,uint32_t),void *,int);
extern void gic_v3_spi_mask(uint32_t);
extern void gic_v3_spi_detach(uint32_t);
static uint32_t fp_sysconf_event_irqs,fp_sysconf_panic_irqs;
static void fp_sysconf_irq(void *context,uint32_t intid) {
    (void)context;gic_v3_spi_mask(intid);
    if(intid==254)__atomic_fetch_add(&fp_sysconf_event_irqs,1,__ATOMIC_RELAXED);
    if(intid==255)__atomic_fetch_add(&fp_sysconf_panic_irqs,1,__ATOMIC_RELAXED);
}
static unsigned fp_sysconf_event_count(void) {return __atomic_load_n(&fp_sysconf_event_irqs,__ATOMIC_RELAXED);}
static unsigned fp_sysconf_panic_count(void) {return __atomic_load_n(&fp_sysconf_panic_irqs,__ATOMIC_RELAXED);}
static bool fp_sysconf_prepare_irqs(void) {
    if(gic_v3_spi_attach_to(254,false,fp_sysconf_irq,NULL,0)!=KERN_SUCCESS)return false;
    if(gic_v3_spi_attach_to(255,false,fp_sysconf_irq,NULL,0)!=KERN_SUCCESS) {
        gic_v3_spi_detach(254);return false;
    }
    if(fp_sysconf_event_count() || fp_sysconf_panic_count()) {
        gic_v3_spi_mask(254);gic_v3_spi_mask(255);return false;
    }
    return true;
}
static void fp_sysconf_prepare_request(void) {
    uint32_t request[3]={254,255,0};
    bzero((void *)fp_channel_pages[0],4096);
    memcpy((void *)fp_channel_pages[0],request,sizeof(request));fp_channel_flush(fp_channel_pages[0]);
}
static void fp_sysconf_read_words(uint32_t words[4]) {
    fp_channel_flush(fp_channel_pages[0]);memcpy(words,(void *)fp_channel_pages[0],16);
}
#include <sys/redwood_fp_sysconf.h>
#endif


static unsigned
parse(const char *s, char *cmd, size_t cmdlen, uint64_t *v, unsigned max)
{
	unsigned n = 0, c = 0;

	while (*s == ' ') {
		s++;
	}
	while (*s && *s != ' ' && c + 1 < cmdlen) {
		cmd[c++] = *s++;
	}
	cmd[c] = 0;
	while (*s && n < max) {
		uint64_t x = 0;
		int base = 10;

		while (*s == ' ') {
			s++;
		}
		if (!*s) {
			break;
		}
		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
			base = 16;
			s += 2;
		}
		for (; *s && *s != ' '; s++) {
			int d = (*s >= '0' && *s <= '9') ? *s - '0' : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10 :
			    (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : 99;

			if (d >= base) {
				break;
			}
			x = x * (uint64_t)base + (uint64_t)d;
		}
		v[n++] = x;
	}
	return n;
}

#ifdef GENERIC_GIC_V2_IRQ
/* Fixed SPI16 byte FIFO bursts, no DMA. Complete input validation before I/O.
 * The caller holds dbgport_mtx; TX requires channels off and enough FIFO room.
 * RX reads exactly the requested bytes from the same FIFO address.
 */
static void
spi8_command(int transmit,const char *hex,uint64_t requested)
{
    uint8_t data[64];
    unsigned count=transmit?spi8_decode(hex,data):(requested>=1&&requested<=64?(unsigned)requested:0);
    if (!count) {snprintf(out_buf,sizeof(out_buf),"spi8: invalid byte count/hex\n");return;}
    volatile uint32_t *usi=(volatile uint32_t *)pmap_pa(0x10721048);
    volatile uint32_t *gate0=(volatile uint32_t *)pmap_pa(0x107020fc);
    volatile uint32_t *gate1=(volatile uint32_t *)pmap_pa(0x1070208c);
    if (!usi || !gate0 || !gate1 || *usi!=2 || !(*gate0&(1u<<21)) || !(*gate1&(1u<<21))) {
        snprintf(out_buf,sizeof(out_buf),"spi8: USI/clock guard refused\n");return;
    }
    volatile uint8_t *base=pmap_pa(0x10960000);
    if (!base) {snprintf(out_buf,sizeof(out_buf),"spi8: unmappable\n");return;}
    uint32_t mode=*(volatile uint32_t *)(base+8),channel=*(volatile uint32_t *)base;
    uint32_t status=*(volatile uint32_t *)(base+0x14);
    if (!spi8_fifo_ready(*usi,mode,channel,status,count,transmit)) {
        snprintf(out_buf,sizeof(out_buf),"spi8: FIFO/mode/channel guard refused\n");return;
    }
    if (transmit) {
        for (unsigned i=0;i<count;i++) *(volatile uint8_t *)(base+0x18)=data[i];
        __asm__ volatile ("dsb sy" ::: "memory");
        snprintf(out_buf,sizeof(out_buf),"spi8.tx: wrote %u\n",count);
    } else {
        size_t offset=0;
        for (unsigned i=0;i<count;i++) {
            uint8_t value=*(volatile uint8_t *)(base+0x1c);
            offset+=(size_t)snprintf(out_buf+offset,sizeof(out_buf)-offset,"%02x",value);
        }
        __asm__ volatile ("dsb sy" ::: "memory");
        snprintf(out_buf+offset,sizeof(out_buf)-offset,"\n");
    }
}
#endif

static void
run(const char *line)
{
	char cmd[32];
	uint64_t v[16] = { 0 };
	unsigned n = parse(line, cmd, sizeof(cmd), v, 16);
	size_t cap = sizeof(out_buf), o = 0;

	out_buf[0] = 0;
#ifdef GENERIC_GIC_V2_IRQ
    if (strcmp(cmd,"fp.tee-sysconf")==0) {fp_tee_sysconf(n==1?v[0]:2);return;}
    if (strcmp(cmd,"fp.tee-channels")==0) {fp_tee_channels(n==1?v[0]:2);return;}
    if (strcmp(cmd,"fp.tee-version")==0) {fp_tee_probe(n==1?v[0]:2);return;}
    if (strcmp(cmd,"cp.memory")==0 && n==0) {cp_memory_audit();return;}
    if (strcmp(cmd,"cp.s2mpu")==0 && n==0) {cp_s2mpu_grant();return;}
    if (strcmp(cmd,"spi8.tx")==0) {
        const char *payload=line;while (*payload==' ') payload++;
        payload+=7;while (*payload==' ') payload++;
        spi8_command(1,payload,0);return;
    }
    if (strcmp(cmd,"spi8.rx")==0) {spi8_command(0,"",n==1?v[0]:0);return;}
#endif
	if (strcmp(cmd, "r") == 0 && n >= 1) {
		unsigned cnt = n >= 2 ? (unsigned)v[1] : 1;

		cnt = cnt > 64 ? 64 : cnt == 0 ? 1 : cnt;
		/* CMU_VTS has reserved holes: a bulk read reached 0x15500608 and aborted. */
		if (cnt > 1 && v[0] < 0x15504000ULL &&
		    (v[0] >= 0x15500000ULL || 0x15500000ULL - v[0] < (uint64_t)cnt * 4)) {
			snprintf(out_buf, cap, "CMU_VTS requires individual documented registers; use mic.cmu after mic.pd\n");
			return;
		}
		for (unsigned i = 0; i < cnt && o < cap; i++) {
			volatile uint8_t *va = pmap_pa(v[0] + 4 * i);

			if (va == NULL) {
				o += (size_t)snprintf(out_buf + o, cap - o, "\nunmappable %#llx", v[0] + 4 * i);
				break;
			}
			if (i % 4 == 0) {
				o += (size_t)snprintf(out_buf + o, cap - o, "%s%#llx:", i ? "\n" : "", v[0] + 4 * i);
			}
			o += (size_t)snprintf(out_buf + o, cap - o, " %08x", *(volatile uint32_t *)va);
		}
		snprintf(out_buf + o, cap - o, "\n");
	} else if ((strcmp(cmd, "w") == 0 && n >= 2) || (strcmp(cmd, "m") == 0 && n >= 3)) {
		volatile uint8_t *va = pmap_pa(v[0]);

		if (va == NULL) {
			snprintf(out_buf, cap, "unmappable %#llx\n", v[0]);
		} else {
			uint32_t old = *(volatile uint32_t *)va, nv = cmd[0] == 'w' ? (uint32_t)v[1] : ((old & (uint32_t)v[1]) | (uint32_t)v[2]);

			*(volatile uint32_t *)va = nv;
			__asm__ volatile ("dsb sy" ::: "memory");
			snprintf(out_buf, cap, "%#llx: %08x -> %08x, reads %08x\n", v[0], old, nv, *(volatile uint32_t *)va);
		}
	} else if (strcmp(cmd, "sr") == 0 && n >= 1) {
		uint64_t x = 0, r = smc(SMC_CMD_REG & 0xffffffffULL, (3U << 30) | ((uint32_t)v[0] >> 2), 0, &x);

		snprintf(out_buf, cap, "%#llx: %08x%s\n", v[0], (uint32_t)x, r == 0 ? "" : " (EL3 refused)");
	} else if (strcmp(cmd, "sw") == 0 && n >= 2) {
		uint64_t r = smc(SMC_CMD_REG, (1U << 30) | ((uint32_t)v[0] >> 2), (uint32_t)v[1], NULL);

		snprintf(out_buf, cap, "%#llx <- %08x through EL3: %lld\n", v[0], (uint32_t)v[1], (long long)r);
	} else {
		for (unsigned i = 0; i < ncmds; i++) {
			if (strcmp(cmd, cmds[i].name) == 0) {
				cmds[i].fn(v, n, out_buf, cap);
				return;
			}
		}
		o = (size_t)snprintf(out_buf, cap, "r PA [N] | w PA V | m PA AND OR | sr PA | sw PA V | help\nfp.tee-version 0|1 (Exynos): inspect or one-shot TEE compatibility; no authentication\n");
		for (unsigned i = 0; i < ncmds && o < cap; i++) {
			o += (size_t)snprintf(out_buf + o, cap - o, "%s %s\n", cmds[i].name, cmds[i].help ? cmds[i].help : "");
		}
	}
}

static int
poke_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char in[256];
	int error = 0;

	lck_mtx_lock(&dbgport_mtx);
	if (req->newptr != USER_ADDR_NULL) {
		size_t len = req->newlen < sizeof(in) - 1 ? req->newlen : sizeof(in) - 1;

		if ((error = SYSCTL_IN(req, in, len)) == 0) {
			in[len] = 0;
			run(in);
		}
	}
	if (error == 0) {
		error = SYSCTL_OUT(req, out_buf, strlen(out_buf) + 1);
	}
	lck_mtx_unlock(&dbgport_mtx);
	return error;
}

SYSCTL_DECL(_kern_redwood);
#ifdef GENERIC_GIC_V2_IRQ
/* Binary, bounded IPC access for a single root bring-up helper. The external
 * phone lock owns the full transaction. This mutex serializes each operation
 * with generic debug mappings. No HVC, power, DMA grant, index reset or firmware
 * command occurs automatically. UNMAP drops only the AP mapping, not reservation.
 */
static int
cp_ipc_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
    struct cp_ipc_request q={.version=CP_IPC_API,.op=CP_IPC_INFO};
    struct cp_ipc_reply reply={.version=CP_IPC_API};
    int error=0;
    size_t input_size=CP_IPC_HEADER;
    if (req->newptr!=USER_ADDR_NULL) {
        input_size=req->newlen;
        if (input_size<CP_IPC_HEADER || input_size>sizeof(q)) return EINVAL;
        if ((error=SYSCTL_IN(req,&q,input_size))) return error;
    }
    if (!cp_ipc_request_valid(&q,input_size)) return EINVAL;
    size_t read_size=(q.op==CP_IPC_READ || q.op==CP_IPC_LOAD32)?q.length:0;
    /* A too-small reply buffer must not cause a hidden partial operation. */
    if (req->oldptr==USER_ADDR_NULL || req->oldlen<CP_IPC_HEADER+read_size) return ENOMEM;
    lck_mtx_lock(&dbgport_mtx);
    if (q.op==CP_IPC_MAP && !cp_ipc_va) {
        extern unsigned long gPhysBase,real_phys_size;
        if (!cp_memory_certified() || !cp_ipc_aperture_disjoint(gPhysBase,real_phys_size)) {
            error=EPERM;goto done;
        }
        for (unsigned i=0;i<nmaps;i++) {
            if (maps[i].page>=CP_IPC_BASE && maps[i].page<CP_IPC_BASE+CP_IPC_SIZE) {
                error=EBUSY;goto done;
            }
        }
        vm_offset_t mapped=ml_io_map_unmappable(CP_IPC_BASE,CP_IPC_SIZE,VM_WIMG_WCOMB);
        if (!mapped) {error=ENOMEM;goto done;}
        for (uint64_t off=0;off<CP_IPC_SIZE;off+=0x1000) {
            if (!cp_memory_par_nc(cp_memory_translation(mapped+off),CP_IPC_BASE+off)) {
                ml_io_unmap(mapped,CP_IPC_SIZE);error=EIO;goto done;
            }
        }
        cp_ipc_va=mapped;
    } else if (q.op==CP_IPC_UNMAP) {
        if (cp_ipc_va) {ml_io_unmap(cp_ipc_va,CP_IPC_SIZE);cp_ipc_va=0;}
        if (cp_pp_va) {ml_io_unmap(cp_pp_va,CP_IPC_PP_SPAN);cp_pp_va=0;}
    } else if (q.op>=CP_IPC_READ) {
        uint8_t *at;
        if (q.offset&CP_IPC_PP_FLAG) {
            /* the packet area: mapped on first use, only with the loader's reservation certificate */
            if (!cp_pp_va) {
                if (!cp_memory_certified()) {error=EPERM;goto done;}
                /* Normal non-cacheable like the IPC window (memcpy may access unaligned: never Device memory);
                 * refused while the debug peek holds a page of it (no mixed-attribute aliases) */
                for (unsigned i=0;i<nmaps;i++)
                    if (maps[i].page>=CP_IPC_PP_BASE && maps[i].page<CP_IPC_PP_BASE+CP_IPC_PP_SPAN) {error=EBUSY;goto done;}
                cp_pp_va=ml_io_map_unmappable((vm_offset_t)CP_IPC_PP_BASE,CP_IPC_PP_SPAN,VM_WIMG_WCOMB);
                if (!cp_pp_va) {error=ENOMEM;goto done;}
            }
            at=(uint8_t *)cp_pp_va+(q.offset&~CP_IPC_PP_FLAG);
        } else {
            if (!cp_ipc_va) {error=ENXIO;goto done;}
            at=(uint8_t *)cp_ipc_va+q.offset;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        switch(q.op) {
        case CP_IPC_READ: memcpy(reply.data,at,q.length);break;
        case CP_IPC_WRITE: memcpy(at,q.data,q.length);break;
        case CP_IPC_LOAD32: {
            uint32_t v=*(volatile uint32_t *)at;memcpy(reply.data,&v,4);break;
        }
        case CP_IPC_STORE32: {
            uint32_t v;memcpy(&v,q.data,4);*(volatile uint32_t *)at=v;break;
        }
        }
        __asm__ volatile("dsb sy" ::: "memory");
    }
    reply.mapped=cp_ipc_va!=0;reply.length=(uint32_t)read_size;
    error=SYSCTL_OUT(req,&reply,CP_IPC_HEADER+read_size);
done:
    lck_mtx_unlock(&dbgport_mtx);
    return error;
}
SYSCTL_PROC(_kern_redwood, OID_AUTO, cp_ipc, CTLTYPE_OPAQUE | CTLFLAG_RW | CTLFLAG_LOCKED,
    NULL, 0, cp_ipc_sysctl, "S,cp_ipc_reply", "root-only bounded retained modem IPC memory");
#endif

extern void dbg_fb_warm(const uint64_t *, unsigned, char *, size_t);

/* backboardd runs as mobile. Expose only the bounded display preference,
 * never the root-only register/debug command interface. */
static int
warm_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char response[256];
	int value = 0, error;

	lck_mtx_lock(&dbgport_mtx);
	dbg_fb_warm(NULL, 0, response, sizeof(response));
	value = strncmp(response, "warm on", 7) == 0;
	error = SYSCTL_OUT(req, &value, sizeof(value));
	if (!error && req->newptr != USER_ADDR_NULL) {
		if (req->newlen != sizeof(value)) error = EINVAL;
		else if (!(error = SYSCTL_IN(req, &value, sizeof(value)))) {
			if (value != 0 && value != 1) error = EINVAL;
			else {
				uint64_t wanted = (uint64_t)value;
				dbg_fb_warm(&wanted, 1, response, sizeof(response));
				if (strncmp(response, value ? "warm on" : "warm off", value ? 7 : 8) != 0) error = EBUSY;
			}
		}
	}
	lck_mtx_unlock(&dbgport_mtx);
	return error;
}
SYSCTL_PROC(_kern_redwood, OID_AUTO, display_warmth,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, NULL, 0, warm_sysctl, "I",
    "optional mild software display warmth (0 or 1)");

/* kern.redwood.display_aod (iokit/Kernel/MiniIOMobileFramebuffer.cpp redwood_fb_aod): the panel dimmed while the
 * Always-On Display shows. SpringBoard (mobile) sets it on the backlight's InactiveOn transition: bounded 0..60, a
 * brightness ceiling only, never a register or power command. */
extern int redwood_fb_aod(int value);
static int
aod_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = redwood_fb_aod(-1), error;

	error = SYSCTL_OUT(req, &value, sizeof(value));
	if (!error && req->newptr != USER_ADDR_NULL) {
		if (req->newlen != sizeof(value)) error = EINVAL;
		else if (!(error = SYSCTL_IN(req, &value, sizeof(value)))) {
			if (value < 0 || value > 60 || redwood_fb_aod(value) != value) error = EINVAL;
		}
	}
	return error;
}
SYSCTL_PROC(_kern_redwood, OID_AUTO, display_aod,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, NULL, 0, aod_sysctl, "I",
    "Always-On Display panel dimming: 0 off, 1 default, 2..60 ceiling in nits");

/* kern.redwood.dvfs_log (bsd/dev/redwood_exynos_acpm.c): every clock change in the log; 0 failures only */
extern uint32_t redwood_dvfs_log;
SYSCTL_UINT(_kern_redwood, OID_AUTO, dvfs_log, CTLFLAG_RW | CTLFLAG_LOCKED, &redwood_dvfs_log, 0,
    "log every DVFS clock change; 0 failures only");

/* kern.redwood.wifi_scan_gap (bsd/dev/redwood_sta.c): broad Wi-Fi scans sooner than this many seconds after the last
 * real one are answered from its results; 0 every scan to the radio */
extern uint32_t redwood_wifi_scan_gap;
SYSCTL_UINT(_kern_redwood, OID_AUTO, wifi_scan_gap, CTLFLAG_RW | CTLFLAG_LOCKED, &redwood_wifi_scan_gap, 0,
    "seconds a broad Wi-Fi scan is answered from the last one; 0 off");

/* kern.redwood.touch_lat: the touch-to-panel latency lines ("tl:", tools/latency.py) live, as boot-arg touch_lat=1 */
extern uint32_t redwood_touch_lat_live;
SYSCTL_UINT(_kern_redwood, OID_AUTO, touch_lat, CTLFLAG_RW | CTLFLAG_LOCKED, &redwood_touch_lat_live, 0,
    "touch-to-panel latency lines (tl:) in the kernel log; 0 off");

/* kern.redwood.wake_stats (osfmk/arm64/psci_smp.c): what wakes the cores, counted at every callout; off by default */
extern uint32_t redwood_wake_stats;
SYSCTL_UINT(_kern_redwood, OID_AUTO, wake_stats, CTLFLAG_RW | CTLFLAG_LOCKED, &redwood_wake_stats, 0,
    "count what wakes each core (the \"wakes core\" lines every 10 s); 0 off");

/* kern.redwood.ksample (osfmk/arm64/user_exception_trace.c): sample a thread where it runs -- 1 SpringBoard's main
 * thread, 2 backboardd's render thread, N process N's main thread, 0 off; "SAMPLE" records in /m2m */
extern int redwood_ksample_get(void);
extern bool redwood_ksample_set(int value);

static int
ksample_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = redwood_ksample_get(), changed = 0;
	int error = sysctl_io_number(req, value, sizeof(value), &value, &changed);

	if (error == 0 && changed && !redwood_ksample_set(value)) {
		error = EINVAL;
	}
	return error;
}
SYSCTL_PROC(_kern_redwood, OID_AUTO, ksample, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, ksample_sysctl, "I",
    "sample a thread where it runs: 1 SpringBoard main, 2 backboardd render, N pid N main, 0 off");

SYSCTL_PROC(_kern_redwood, OID_AUTO, poke, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, poke_sysctl, "A",
    "the debug port: r/w/m/sr/sw and the drivers' commands (help)");
