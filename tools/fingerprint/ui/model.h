/* UI-only state. No state grants authentication or unlocks a keybag. */
#ifndef RW_FP_UI_MODEL_H
#define RW_FP_UI_MODEL_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
enum rw_fp_phase { RW_IDLE, RW_REGISTER, RW_COMPARE, RW_SAVED, RW_REJECT, RW_ERROR, RW_UNVERIFIED };
struct rw_fp_ui { enum rw_fp_phase phase; unsigned progress; bool saved; uint64_t event; };
static void rw_fp_line(struct rw_fp_ui *s, const char *line) {
    const char *p=strstr(line,"redwood-tee: ");
    if(!p)return;
    p+=strlen("redwood-tee: ");
    if(s->phase==RW_REGISTER) {
        unsigned n,v;
        if(sscanf(p,"fp run 3: SAMPLE %u ACCEPTED: progress %u",&n,&v)==2 && v<=100) {
            s->progress=v; s->event++;
        }
        /* Progress100 alone does not prove that saving succeeded. */
        unsigned bytes; int error;
        if(s->progress==100 && sscanf(p,"fp template: saved %u bytes to /var/root/fp-template.bin (error %d)",&bytes,&error)==2) {
            s->phase=(bytes>0 && bytes<=0x64000 && error==0)?RW_SAVED:RW_ERROR;
            s->saved=s->phase==RW_SAVED; s->event++;
        }
    } else if(s->phase==RW_COMPARE || s->phase==RW_REJECT || s->phase==RW_UNVERIFIED) {
        if(!strcmp(p,"fp run 6: NO MATCH")) {s->phase=RW_REJECT;s->event++;}
        if(!strncmp(p,"fp run 6: SENSOR MATCH candidate",31)) {s->phase=RW_UNVERIFIED;s->event++;}
    }
    if(strstr(p,"invalid response") || strstr(p,"re-arm failed") || strstr(p,"capture error 0x44")) {
        s->phase=RW_ERROR;s->event++;
    }
}
static const char *rw_fp_phase_name(enum rw_fp_phase p) {
    switch(p){case RW_REGISTER:return "registering";case RW_COMPARE:return "comparing";
    case RW_SAVED:return "saved";case RW_REJECT:return "rejected";case RW_ERROR:return "unavailable";
    case RW_UNVERIFIED:return "unverified";default:return "idle";}
}
#endif
