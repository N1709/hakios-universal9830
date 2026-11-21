#include "model.h"
#include <assert.h>
int main(void) {
    assert(!strcmp(rw_fp_phase_name(RW_UNVERIFIED),"unverified"));
    struct rw_fp_ui s={.phase=RW_REGISTER};
    rw_fp_line(&s,"redwood-tee: fp run 3: SAMPLE 17 ACCEPTED: progress 100 | omitted");
    assert(s.progress==100 && !s.saved && s.phase==RW_REGISTER);
    rw_fp_line(&s,"redwood-tee: fp template: saved 376182 bytes to /var/root/fp-template.bin (error 5)");
    assert(s.phase==RW_ERROR && !s.saved);
    s=(struct rw_fp_ui){.phase=RW_REGISTER,.progress=100};
    rw_fp_line(&s,"redwood-tee: fp template: saved 376182 bytes to /var/root/fp-template.bin (error 0)");
    assert(s.saved && s.phase==RW_SAVED);
    s.phase=RW_COMPARE;
    rw_fp_line(&s,"redwood-tee: fp run 6: finger held on the sensor: capture");
    assert(s.phase==RW_COMPARE);
    rw_fp_line(&s,"redwood-tee: fp run 6: NO MATCH");assert(s.phase==RW_REJECT);
    rw_fp_line(&s,"redwood-tee: fp run 6: SENSOR MATCH candidate (unverified)");assert(s.phase==RW_UNVERIFIED);
    rw_fp_line(&s,"redwood-tee: fp run 6: invalid response");assert(s.phase==RW_ERROR);
    s=(struct rw_fp_ui){.phase=RW_IDLE};
    rw_fp_line(&s,"redwood-tee: fp run 6: NO MATCH");assert(s.phase==RW_IDLE);
    puts("PASS: progress/save failure, real rejection, unverified candidate, inactive event isolation");
}
