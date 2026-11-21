/* S20 lens selection; stock-style0.5 ultra-wide UI scale; no per-device focal calibration. */
#ifndef CAMERA_S20_ZOOM_H
#define CAMERA_S20_ZOOM_H
#include <stdbool.h>
#include <math.h>
static inline float s20_zoom_clamp(float z,bool rear,bool enabled){
    float min=rear&&enabled?0.5f:1.0f;
    if(!isfinite(z))return 1.0f;
    return z<min?min:z>16?16:z;
}
static inline bool s20_sensor_is_rear(unsigned sensor){return sensor==1||sensor==3||sensor==4;}
static inline unsigned s20_zoom_mode(float z,bool enabled){return enabled&&z<1?5:2;}
/* Provisional3x entry; opt-in until sensor/FOV/quality are qualified. */
static inline unsigned s20_zoom_mode_tele(float z,bool ultra,bool tele){return tele&&z>=3?6:s20_zoom_mode(z,ultra);}
static inline float s20_zoom_crop_sensor(float z,unsigned sensor){return sensor==4?fmaxf(1,z/2.0f):sensor==3?z/0.5f:z;}
static inline float s20_zoom_crop(float z,bool ultra){return ultra?z/0.5f:z;}
/* Map a displayed primary ROI back into its centre-cropped RAW frame. */
static inline void s20_zoom_window(const float input[4],float output[4],float zoom){
    if(!isfinite(zoom)||zoom<1)zoom=1;
    for(unsigned i=0;i<4;i++){
        float v=isfinite(input[i])?input[i]:(i<2?0.3f:0.7f);
        v=v<0?0:v>1?1:v;output[i]=zoom==1?v:0.5f+(v-0.5f)/zoom;
    }
}
#endif
