/* Evaluate the production black interpolation against an independent covered RAW.
 * Host only: no device access. Signed residuals are measured BEFORE clipping. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../../userland/src/camera_isp.c"
int main(int argc, char **argv) {
    if (argc!=3) return 2;
    struct isp_black_calibration cal;
    struct isp isp;
    isp_init(&isp);
    if (!isp_load_black_calibration(&cal,argv[1])) return 2;
    FILE *fp=fopen(argv[2],"rb");
    if (!fp) return 2;
    struct redwood_camera_frame f;
    if (fread(&f,sizeof(f),1,fp)!=1 || memcmp(f.magic,"RWFRAME1",8) || f.header_bytes!=sizeof(f) ||
        f.width!=2016 || f.height!=1512 || f.stride!=2528) return 2;
    size_t bytes=(size_t)f.stride*f.height;
    uint8_t *data=malloc(bytes);
    if (!data || fread(data,1,bytes,fp)!=bytes || fgetc(fp)!=EOF) return 2;
    fclose(fp);
    if (!isp_apply_black_calibration(&isp,&cal,&f)) return 2;
    double sum[4]={0},sq[4]={0},old[4]={0},old_sq[4]={0};
    unsigned n[4]={0};
    double max_tile=0,old_max_tile=0;
    /* Existing production inside-table, same interpolation/rounding as camera_device.c. */
    const float gains[4]={1,4,8,16};
    const float inside[4][4]={{57.9f,40.3f,28.1f,18.1f},{59.4f,44.7f,39,30},
        {56,39,30.4f,25},{59.2f,44.1f,36.8f,30}};
    float gain=(float)f.gain/f.gain_unity;
    unsigned seg=gain<=4?0:gain<=8?1:2;
    float t=(gain-gains[seg])/(gains[seg+1]-gains[seg]);
    double previous[4];
    for(unsigned k=0;k<4;k++) previous[k]=(int)(inside[k][seg]+t*(inside[k][seg+1]-inside[k][seg])+0.5f);
    double old_bias_sq[4]={0},new_bias_sq[4]={0};
    unsigned tiles=0;
    for(unsigned by=0;by<13;by++) for(unsigned bx=0;bx<17;bx++) {
        double tile[4]={0},old_tile[4]={0}; unsigned count[4]={0};
        unsigned y0=(f.height*by/13)&~1u,y1=(f.height*(by+1)/13)&~1u;
        unsigned x0=(f.width*bx/17)&~3u,x1=(f.width*(bx+1)/17)&~3u;
        for(unsigned y=y0;y<y1;y+=4) for(unsigned x=x0;x<x1;x+=8) {
            for(unsigned dy=0;dy<2;dy++) for(unsigned dx=0;dx<2;dx++) {
                unsigned k=dy*2+dx;
                const uint8_t *g=data+(y+dy)*f.stride+(x/4)*5;
                int v=(g[dx]<<2)|((g[4]>>(dx*2))&3);
                double residual=v-black_at(&isp,&f,x+dx,y+dy);
                sum[k]+=residual; sq[k]+=residual*residual; old[k]+=v-previous[k]; old_sq[k]+=(v-previous[k])*(v-previous[k]); old_tile[k]+=v-previous[k]; n[k]++;
                tile[k]+=residual; count[k]++;
            }
        }
        tiles++;
        for(unsigned k=0;k<4;k++) {
            old_bias_sq[k]+=(old_tile[k]/count[k])*(old_tile[k]/count[k]);
            new_bias_sq[k]+=(tile[k]/count[k])*(tile[k]/count[k]);
            if(fabs(tile[k]/count[k])>max_tile) max_tile=fabs(tile[k]/count[k]);
            if(fabs(old_tile[k]/count[k])>old_max_tile) old_max_tile=fabs(old_tile[k]/count[k]);
        }
    }
    for(unsigned k=0;k<4;k++) printf("site %u: old bias %.3f -> corrected %.3f; RMS %.3f -> %.3f\n",k,old[k]/n[k],sum[k]/n[k],sqrt(old_sq[k]/n[k]),sqrt(sq[k]/n[k]));
    printf("maximum absolute tile mean: old %.3f -> corrected %.3f RAW counts\n",old_max_tile,max_tile);
    bool improved=true;
    for(unsigned k=0;k<4;k++) {
        printf("site %u spatial bias RMS %.3f -> %.3f\n",k,sqrt(old_bias_sq[k]/tiles),sqrt(new_bias_sq[k]/tiles));
        if(new_bias_sq[k] > old_bias_sq[k]*0.25 || fabs(sum[k]/n[k])>2) improved=false;
    }
    free(data);
    /* Improvement gate, NOT full-quality acceptance: at least 50% spatial-bias
     * RMS reduction per site, mean bias below two RAW counts. High-gain random
     * noise is reported separately and is not claimed solved by a dark map. */
    return improved ? 0:1;
}
