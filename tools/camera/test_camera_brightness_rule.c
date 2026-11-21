#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../../userland/src/camera_brightness_rule.h"
int main(void){
    struct cb_state s={0};
    struct cb_input in={.camera=true,.enabled=true,.display_on=1,.automatic=1,.ui=.42,.now=10};
    struct cb_plan p=cb_plan_step(s,in);
    assert(p.save&&p.set_auto&&!p.auto_value&&p.set_ui&&p.ui==.7&&p.next.prior_auto);
    assert(p.next.prior_ui==.42);s=p.next;
    in.now=10.5;in.ui=.42;in.automatic=0;
    p=cb_plan_step(s,in);assert(!p.save&&!p.set_ui); /* asynchronous own write grace */
    in.now=11.1;in.ui=.7;p=cb_plan_step(s,in);assert(!p.save&&!p.set_ui&&!p.set_auto);
    in.ui=.55;p=cb_plan_step(s,in);assert(p.save&&p.next.manual&&p.next.target_ui==.55&&!p.set_ui);s=p.next;
    in.camera=false;p=cb_plan_step(s,in);assert(p.clear&&p.set_auto&&p.auto_value&&p.set_ui&&p.ui==.55&&!p.next.held);
    s=(struct cb_state){0};in=(struct cb_input){.camera=true,.enabled=true,.display_on=1,.automatic=0,.ui=.3,.now=20};
    p=cb_plan_step(s,in);s=p.next;assert(!s.prior_auto);
    in.camera=false;p=cb_plan_step(s,in);assert(p.clear&&!p.auto_value&&p.ui==.3);
    in.camera=true;in.display_on=0;p=cb_plan_step(s,in);assert(p.clear&&!p.auto_value&&p.ui==.3);
    in.display_on=1;in.enabled=false;p=cb_plan_step(s,in);assert(p.clear);
    in.enabled=true;in.automatic=-1;p=cb_plan_step(s,in);assert(!p.clear&&!p.save&&!p.set_auto&&!p.set_ui);
    in.automatic=0;in.ui=NAN;p=cb_plan_step(s,in);assert(!p.clear&&!p.save);
    assert(cb_lease_valid(100,99)&&!cb_lease_valid(100,98)&&!cb_lease_valid(100,101));
    assert(!cb_lease_valid(100,0)&&!cb_lease_valid(NAN,99)&&!cb_lease_valid(100,NAN));
    puts("camera display policy: enter/steady/manual/exit, auto-off preservation, display-off, disabled rule, unknown service and stale/crashed lease PASS");
}
