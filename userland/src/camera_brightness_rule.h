/* Camera-only display policy. No camera AE/AF or authentication code. */
#ifndef REDWOOD_CAMERA_BRIGHTNESS_RULE_H
#define REDWOOD_CAMERA_BRIGHTNESS_RULE_H
#include <stdbool.h>
#include <math.h>
struct cb_state { bool held, prior_auto, manual; double prior_ui, target_ui, entered; };
struct cb_input { bool camera, enabled; int display_on, automatic; double ui, now; };
struct cb_plan { struct cb_state next; bool save, clear, set_auto, auto_value, set_ui; double ui; };
static inline bool cb_lease_valid(double now,double stamp) {
    return isfinite(now)&&isfinite(stamp)&&stamp>0&&now>=stamp&&now-stamp<2;
}
static inline struct cb_plan cb_plan_step(struct cb_state state,struct cb_input in) {
    struct cb_plan p={.next=state};
    if(in.display_on<0||in.automatic<0||!isfinite(in.ui)||in.ui<0||in.ui>1||!isfinite(in.now))return p;
    bool camera=in.enabled&&in.camera&&in.display_on==1;
    if(!state.held&&camera) {
        p.next=(struct cb_state){.held=true,.prior_auto=in.automatic!=0,.prior_ui=in.ui,.target_ui=.7,.entered=in.now};
        p.save=p.set_auto=p.set_ui=true;p.auto_value=false;p.ui=.7;
    } else if(state.held&&!camera) {
        p.set_auto=p.set_ui=p.clear=true;p.auto_value=state.prior_auto;
        p.ui=state.manual?state.target_ui:state.prior_ui;p.next.held=false;
    } else if(state.held&&camera) {
        if(in.automatic!=0){p.set_auto=true;p.auto_value=false;}
        /* Initial write may be asynchronous. Afterwards, the user's legacy
         * slider owns its value; never repeatedly force70 against that input. */
        if(in.now-state.entered>=1&&fabs(in.ui-state.target_ui)>.005) {
            p.next.manual=true;p.next.target_ui=in.ui;p.save=true;
        }
    }
    return p;
}
#endif
