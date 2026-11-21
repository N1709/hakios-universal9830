/* One-shot TEEGRIS compatibility diagnostic, not biometric authentication.
 * ABI numbers observed in Samsung's Exynos990 tzdev_internal.h and Kconfig:
 * ARM fast SMC32, trusted OS owner 50, CHECK_VERSION 13, driver ABI 3.0.0.
 * The Linux version field is the reference client's ABI value (4.19.87),
 * not a claim this kernel is Linux. No shared pages, GPIO or storage writes.
 * Requires an explicit root debug-port command; never runs during boot.
 */
#ifndef SYS_REDWOOD_FP_PROBE_H
#define SYS_REDWOOD_FP_PROBE_H
#define FP_TEE_CHECK_VERSION 0xb200000dULL
#define FP_TEE_REFERENCE_CLIENT 0x00041357ULL
#define FP_TEE_DRIVER_ABI 0x00030000ULL
static bool fp_tee_probe_attempted;
static uint64_t fp_tee_probe_result, fp_tee_probe_x2;

static void
fp_tee_probe(uint64_t armed)
{
    if (armed > 1) {
        snprintf(out_buf,sizeof(out_buf),"fp.tee-version: use 0 to inspect, 1 for one compatibility call\n");
        return;
    }
    if (!armed && !fp_tee_probe_attempted) {
        snprintf(out_buf,sizeof(out_buf),"fp.tee-version: NOT RUN; command 0xb200000d, reference-client 4.19.87, driver ABI 3.0.0; no matching or enrollment\n");
        return;
    }
    bool cached=fp_tee_probe_attempted;
    if (!fp_tee_probe_attempted) {
        fp_tee_probe_attempted=true; /* mark before calling; no automatic retry */
        fp_tee_probe_result=fp_tee_smc(FP_TEE_CHECK_VERSION,FP_TEE_REFERENCE_CLIENT,FP_TEE_DRIVER_ABI,&fp_tee_probe_x2);
    }
    snprintf(out_buf,sizeof(out_buf),"fp.tee-version: %s x0=%#llx x2=%#llx; compatibility result only, no fingerprint match\n",
        cached?"cached":"called once",fp_tee_probe_result,fp_tee_probe_x2);
}
#endif
