/*
 * x1slte (SM-G980F, 2026-11-06): the USB console's commands (redwood_dwc3.c, from Scion's hakios.dwc3.c) that
 * reach drivers of the Galaxy S9 this tree does not have. Each says so; nothing else.
 */
#include <stdbool.h>
#include <stdint.h>
#include <sys/errno.h>
#include <sys/systm.h>

#define NOT_HERE(name) printf("usb> %s: not on this phone\n", name)

void redwood_abox_console(__unused const char *a) { NOT_HERE("abox"); }
void redwood_bt_console(__unused const char *a) { NOT_HERE("bt"); }
void redwood_cs47l93_console(__unused const char *a) { NOT_HERE("codec"); }
void redwood_ssp_console(__unused const char *a) { NOT_HERE("shub"); }
void redwood_sta_console_scan(void) { NOT_HERE("wifi scan"); }
void redwood_wifi_report(__unused bool first) { NOT_HERE("wifi"); }
void redwood_net_console_ip(void) { NOT_HERE("wifi ip"); }
void redwood_net_console_ping(__unused const char *a) { NOT_HERE("wifi ping"); }
void redwood_net_console_dns(__unused const char *a) { NOT_HERE("wifi dns"); }
void redwood_net_console_tcp(__unused const char *a) { NOT_HERE("wifi tcp"); }
void redwood_net_console_stat(void) { NOT_HERE("wifi stat"); }
void redwood_net_console_names(__unused const char *a) { NOT_HERE("wifi names"); }
void redwood_input_console_touch(__unused const char *a) { NOT_HERE("touch"); }
void redwood_input_console_button(__unused const char *a) { NOT_HERE("button"); }
void redwood_ufs_summary(__unused const char *when, __unused int how) { }
void redwood_exynos_cpufreq_touch(void) { }
int redwood_panel_brightness(__unused int level) { return ENODEV; }
int redwood_fb_scanned(__unused uintptr_t *b, __unused uint32_t *w, __unused uint32_t *h, __unused uint32_t *r,
    __unused unsigned *i) { return ENODEV; }
const char *redwood_exynos_camera_shot(__unused uint32_t n, __unused int p) { return "no camera driver on this phone"; }
const char *redwood_exynos_iris_shot(__unused uint32_t n) { return "no iris camera on this phone"; }
/* the USB-C switch: the MUIC's firmware leaves D+/D- on the AP's USB (UEFI's fastboot works with no MUIC driver) */
void redwood_max77705_usb_close(void) { }
void redwood_max77705_usb_console(__unused const char *a) { NOT_HERE("usb muic"); }
