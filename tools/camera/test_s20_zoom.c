#include <assert.h>
#include <stdio.h>
#include "../../userland/src/camera_s20_zoom.h"
int main(void){
 assert(s20_zoom_clamp(.6f,false,true)==1); /* front remains >=1 */
 assert(s20_zoom_clamp(.6f,true,false)==1); /* feature disabled */
 assert(s20_zoom_clamp(.4f,true,true)==.5f);
 assert(s20_zoom_clamp(INFINITY,true,true)==1);
 assert(s20_zoom_clamp(NAN,true,true)==1);
 assert(s20_zoom_clamp(20,true,true)==16);
 assert(s20_zoom_mode(.6f,true)==5&&s20_zoom_mode(.99f,true)==5);
 assert(s20_zoom_mode(1,true)==2&&s20_zoom_mode(.6f,false)==2);
 assert(fabsf(s20_zoom_crop(.5f,true)-1)<1e-6f);
 assert(fabsf(s20_zoom_crop(.9f,true)-1.8f)<1e-6f);
 assert(s20_zoom_crop(2,false)==2);
 float window[4]={0,.2f,.4f,.8f},mapped[4];s20_zoom_window(window,mapped,2);
 assert(fabsf(mapped[0]-.25f)<1e-6f&&fabsf(mapped[2]-.45f)<1e-6f);
 s20_zoom_window(window,mapped,1);for(unsigned i=0;i<4;i++)assert(mapped[i]==window[i]);
 assert(s20_sensor_is_rear(1)&&s20_sensor_is_rear(3)&&s20_sensor_is_rear(4)&&!s20_sensor_is_rear(2));
 assert(s20_zoom_mode_tele(2.99f,true,true)==2&&s20_zoom_mode_tele(3,true,true)==6);
 assert(s20_zoom_mode_tele(5,true,false)==2&&s20_zoom_mode_tele(.5f,true,true)==5);
 assert(s20_zoom_crop_sensor(5,4)==2.5f&&s20_zoom_crop_sensor(10,4)==5);
 puts("PASS: front/disabled zoom, finite clamp, lens boundary and per-lens crop");
}
