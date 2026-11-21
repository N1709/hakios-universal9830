/* Experimental first TEEGRIS transport handshake. No trustlet, sensor or auth.
 * Included after fixed platform helpers. Pages remain wired until reboot once
 * exposed to firmware, including on errors; never reclaim a potentially shared
 * page while the secure world may still reference it.
 */
#ifndef SYS_REDWOOD_FP_CHANNELS_H
#define SYS_REDWOOD_FP_CHANNELS_H
#define FP_TEE_CONNECT_AUX 0xb2000000ULL
#define FP_TEE_CONNECT 0xb2000001ULL
#define FP_TEE_SERVICE_MODE 1u
#define FP_TEE_MAX_CPUS 8u
static bool fp_channels_attempted;
static unsigned fp_channels_registered;
static uint64_t fp_channels_result;
static bool fp_channels_service;
static uint32_t fp_channels_cpu_mask, fp_channels_user_mask;

static void
fp_tee_channels(uint64_t armed)
{
    if (armed > 1) {
        snprintf(out_buf,sizeof(out_buf),"fp.tee-channels: use 0 to inspect, 1 for one AUX/service handshake\n");
        return;
    }
    if (!armed && !fp_channels_attempted) {
        snprintf(out_buf,sizeof(out_buf),"fp.tee-channels: NOT RUN; AUX per online CPU and one service page, no matching or enrollment\n");
        return;
    }
    if (fp_channels_attempted) goto report;
    if (!fp_tee_probe_attempted || fp_tee_probe_result) {
        snprintf(out_buf,sizeof(out_buf),"fp.tee-channels: REFUSED; successful explicit version probe required\n");
        return;
    }
    unsigned cpus=fp_channel_cpu_count();
    if (!cpus || cpus>FP_TEE_MAX_CPUS) {
        snprintf(out_buf,sizeof(out_buf),"fp.tee-channels: REFUSED; unsupported CPU count\n");return;
    }
    for (unsigned cpu=0;cpu<cpus;cpu++) {
        if (!fp_channel_online(cpu)) {
            snprintf(out_buf,sizeof(out_buf),"fp.tee-channels: REFUSED; all configured CPUs must be online\n");return;
        }
    }
    fp_channels_attempted=true;
    /* Allocate and clear all private backing pages before the first SMC.
     * The wire PFN is always physical >> 12, never the XNU 16-KiB PFN. */
    uint32_t pfns[FP_TEE_MAX_CPUS+1];
    for (unsigned i=0;i<=cpus;i++) {
        if (!fp_channel_allocate(i,&pfns[i])) {fp_channels_result=0xfffffff4ULL;goto report;}
    }
    for (unsigned cpu=0;cpu<cpus;cpu++) {
        fp_channels_result=fp_channel_call(cpu,FP_TEE_CONNECT_AUX,pfns[cpu],0);
        if (fp_channels_result)goto report;
        fp_channels_registered++;
    }
    /* Samsung's service channel is mode 1, one 4-KiB page. Its PFN list is
     * marshalled in CPU0's AUX buffer using the reference 32-bit PFN ABI. */
    fp_channel_service_prepare(pfns[cpus]);
    fp_channels_result=fp_channel_call(0,FP_TEE_CONNECT,FP_TEE_SERVICE_MODE,1);
    if (!fp_channels_result) {
        fp_channels_service=true;
        fp_channel_service_read(&fp_channels_cpu_mask,&fp_channels_user_mask);
    }
report:
    snprintf(out_buf,sizeof(out_buf),"fp.tee-channels: retained AUX=%u service=%u result=%#llx cpu_mask=%#x user_mask=%#x; no matching or enrollment\n",
        fp_channels_registered,fp_channels_service?1u:0u,fp_channels_result,fp_channels_cpu_mask,fp_channels_user_mask);
}
#endif
