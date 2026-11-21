/*
 * x1slte (SM-G980F, 2026-11-06): what Scion's sys/hakios.log.h and sys/hakios.xnu.h gave hakios.dwc3.c, for its
 * copy here (redwood_dwc3.c). say() is printf: it lands in the loader's log, which the tap streams to the host.
 */
#ifndef _REDWOOD_USB_COMPAT_H_
#define _REDWOOD_USB_COMPAT_H_
#include <mach/mach_types.h>
#include <stdint.h>
#include <sys/systm.h>

#define say printf

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern kern_return_t thread_terminate(thread_t thread);
extern void thread_set_thread_name(thread_t thread, const char *name);
extern void delay(int usec);
extern void IOSleep(unsigned int milliseconds);
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern int kernel_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
extern void loader_log_line(const char *line);
extern void loader_return(void);
#endif
