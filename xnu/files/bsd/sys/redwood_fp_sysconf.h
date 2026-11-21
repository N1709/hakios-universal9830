/* Explicit, one-shot TEEGRIS SYSCONF exchange. No scheduler or matcher.
 * Protocol: normal-world event IRQ, panic IRQ, flags (three LE uint32s);
 * response begins with OS version, CPU count, big mask and flags.
 */
#ifndef SYS_REDWOOD_FP_SYSCONF_H
#define SYS_REDWOOD_FP_SYSCONF_H
#define FP_TEE_SYSCONF 0xb2000009ULL
static bool fp_sysconf_attempted,fp_sysconf_valid;
static uint64_t fp_sysconf_result;
static uint32_t fp_sysconf_words[4];
static void fp_tee_sysconf(uint64_t armed) {
    if(armed>1){snprintf(out_buf,sizeof(out_buf),"fp.tee-sysconf: use 0 to inspect, 1 to exchange once\n");return;}
    if(!armed && !fp_sysconf_attempted) {
        snprintf(out_buf,sizeof(out_buf),"fp.tee-sysconf: NOT RUN; reserved IRQs 254/255 and bounded configuration exchange only\n");return;
    }
    if(fp_sysconf_attempted)goto report;
    if(!fp_channels_attempted || !fp_channels_service || fp_channels_result ||
        fp_channels_registered!=fp_channel_cpu_count()) {
        snprintf(out_buf,sizeof(out_buf),"fp.tee-sysconf: REFUSED; successful complete channel handshake required\n");return;
    }
    fp_sysconf_attempted=true;
    if(!fp_sysconf_prepare_irqs()){fp_sysconf_result=0xfffffff0ULL;goto report;}
    fp_sysconf_prepare_request();
    fp_sysconf_result=fp_channel_call(0,FP_TEE_SYSCONF,0,0);
    if(!fp_sysconf_result) {
        fp_sysconf_read_words(fp_sysconf_words);
        uint32_t cpus=fp_sysconf_words[1];
        fp_sysconf_valid=cpus && cpus<=FP_TEE_MAX_CPUS &&
            !(fp_sysconf_words[2] & ~((1u<<cpus)-1u));
    }
report:
    if(fp_sysconf_panic_count())fp_sysconf_valid=false;
    snprintf(out_buf,sizeof(out_buf),"fp.tee-sysconf: result=%#llx valid=%u os=%#x cpus=%u big_mask=%#x flags=%#x event_irqs=%u panic_irqs=%u; no scheduling or fingerprint match\n",
        fp_sysconf_result,fp_sysconf_valid?1u:0u,fp_sysconf_words[0],fp_sysconf_words[1],
        fp_sysconf_words[2],fp_sysconf_words[3],fp_sysconf_event_count(),fp_sysconf_panic_count());
}
#endif
