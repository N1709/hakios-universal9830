/* Synthetic RAW10 regression: black correction follows the frame snapshot. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "camera_isp.h"

#define W 8
#define H 8
#define STRIDE 16
#define OUT 12
static struct redwood_camera_frame frame = {
    .width=W, .height=H, .stride=STRIDE, .bayer=0, .black=64, .sequence=1
};

static void raw_frame(uint8_t *raw, const struct isp *isp)
{
    memset(raw, 0, STRIDE*H);
    for (unsigned y=0; y<H; y++) {
        const int *bl=isp->black.band_row && y>=isp->black.band_row ? isp->black.low : isp->black.site;
        for (unsigned x=0; x<W; x+=4) {
            uint8_t *g=raw+y*STRIDE+(x/4)*5;
            for (unsigned k=0; k<4; k++) {
                /* Non-neutral signals exercise the four CFA sites separately. */
                unsigned site=(y&1)*2+((x+k)&1);
                unsigned v=bl[site]+(unsigned[]){80,120,120,160}[site];
                g[k]=v>>2; g[4]|=(v&3)<<(k*2);
            }
        }
    }
}

static void render(const struct isp *isp, const uint8_t *raw, uint8_t *out)
{
    isp_render_bgra(isp,&frame,raw,out,OUT*4,OUT,OUT,0,OUT);
}

struct thread_job { struct isp snapshot; const uint8_t *raw; uint8_t expected[OUT*OUT*4]; };
static void *draw_thread(void *arg)
{
    struct thread_job *job=arg;
    for (unsigned i=0;i<100;i++) {
        uint8_t out[OUT*OUT*4];
        render(&job->snapshot,job->raw,out);
        assert(memcmp(out,job->expected,sizeof(out))==0);
    }
    return NULL;
}

int main(void)
{
    struct isp reader, flat;
    isp_init(&reader); isp_init(&flat);
    uint8_t banded[STRIDE*H], uniform[STRIDE*H];
    int upper[4]={20,30,40,50},lower[4]={8,12,16,20};
    memcpy(reader.black.site,upper,sizeof(upper));
    memcpy(reader.black.low,lower,sizeof(lower));
    reader.black.band_row=4;
    raw_frame(banded,&reader); raw_frame(uniform,&flat);
    struct isp snapshot=reader;
    uint8_t expected[OUT*OUT*4],out[OUT*OUT*4];
    render(&flat,uniform,expected);
    render(&snapshot,banded,out);
    assert(memcmp(out,expected,sizeof(out))==0);

    /* Reading/configuring frame n+1 cannot change frame n's saved correction. */
    for (unsigned i=0;i<4;i++) reader.black.site[i]=reader.black.low[i]=127;
    reader.black.band_row=0;
    render(&reader,banded,out); assert(memcmp(out,expected,sizeof(out))!=0);
    /* Same raw address/sequence on this thread: row cache must include black state. */
    render(&snapshot,banded,out); assert(memcmp(out,expected,sizeof(out))==0);

    double green_a,green_b;
    assert(isp_window_green(&snapshot,&frame,banded,0,0,1,1,&green_a)>0);
    assert(isp_window_green(&flat,&frame,uniform,0,0,1,1,&green_b)>0);
    assert(green_a==120 && green_a==green_b);
    assert(isp_window_green(&snapshot,&frame,banded,-1,0,1,1,&green_a)==0 && green_a==0);

    struct isp_stats a,b;
    isp_measure(&snapshot,&frame,banded,&a); isp_measure(&flat,&frame,uniform,&b);
    for(unsigned c=0;c<3;c++) assert(a.mean[c]==b.mean[c]);
    frame.black=300;
    isp_measure(NULL,&frame,uniform,&a);
    assert(a.mean[0]==0 && a.mean[1]==0 && a.mean[2]==0);
    frame.black=64;

    uint8_t fy[W*H],fuv[W*H/2],by[W*H],buv[W*H/2];
    isp_render_full_nv12(&flat,&frame,uniform,fy,W,fuv,W,0,H,false);
    isp_render_full_nv12(&snapshot,&frame,banded,by,W,buv,W,0,H,false);
    assert(memcmp(fy,by,sizeof(fy))==0 && memcmp(fuv,buv,sizeof(fuv))==0);
    isp_render_still_nv12(&flat,&frame,uniform,fy,W,fuv,W,W,H,0,H,false);
    isp_render_still_nv12(&snapshot,&frame,banded,by,W,buv,W,W,H,0,H,false);
    assert(memcmp(fy,by,sizeof(fy))==0 && memcmp(fuv,buv,sizeof(fuv))==0);

    /* A private spatial map must reach preview/stats/full/still with the same CFA offsets. */
    struct isp spatial=snapshot;
    spatial.black.spatial=true;
    for(unsigned k=0;k<4;k++) for(unsigned yy=0;yy<BLACK_GRID_H;yy++)
        for(unsigned xx=0;xx<BLACK_GRID_W;xx++) spatial.black.grid[k][yy][xx]=(float)upper[k];
    struct isp one_band=snapshot;
    one_band.black.band_row=0;
    uint8_t one_raw[STRIDE*H]; raw_frame(one_raw,&one_band);
    render(&spatial,one_raw,out); assert(memcmp(out,expected,sizeof(out))==0);
    isp_measure(&spatial,&frame,one_raw,&a); isp_measure(&flat,&frame,uniform,&b);
    for(unsigned k=0;k<3;k++) assert(a.mean[k]==b.mean[k]);
    isp_render_full_nv12(&spatial,&frame,one_raw,by,W,buv,W,0,H,false);
    assert(memcmp(fy,by,sizeof(fy))==0 && memcmp(fuv,buv,sizeof(fuv))==0);
    isp_render_still_nv12(&spatial,&frame,one_raw,by,W,buv,W,W,H,0,H,false);
    assert(memcmp(fy,by,sizeof(fy))==0 && memcmp(fuv,buv,sizeof(fuv))==0);

    struct isp_black_calibration cal={.width=2016,.height=1512,.bayer=1,.line_ns=1468,
        .frame_lines=22564,.exposure_lines=20436,.profiles=2,.gain_unity=32};
    cal.profile[0].gain=32; cal.profile[1].gain=128;
    for(unsigned k=0;k<4;k++) for(unsigned yy=0;yy<BLACK_GRID_H;yy++)
        for(unsigned xx=0;xx<BLACK_GRID_W;xx++) {
            cal.profile[0].grid[k][yy][xx]=20;
            cal.profile[1].grid[k][yy][xx]=44;
        }
    struct redwood_camera_frame cf={.sensor=1,.format=1,.width=2016,.height=1512,.bayer=1,
        .line_ns=1468,.frame_lines=22564,.exposure_lines=20436,.gain_unity=32,.gain=80};
    assert(isp_apply_black_calibration(&spatial,&cal,&cf));
    assert(spatial.black.grid[0][0][0]==32);
    cf.sensor=2; assert(!isp_apply_black_calibration(&spatial,&cal,&cf) && !spatial.black.spatial);
    cf.sensor=1; cf.exposure_lines--; assert(!isp_apply_black_calibration(&spatial,&cal,&cf));
    cf.exposure_lines++; cf.gain=129; assert(!isp_apply_black_calibration(&spatial,&cal,&cf));
    cf.gain=31; assert(!isp_apply_black_calibration(&spatial,&cal,&cf));

    /* Loader rejects malformed/trailing input private calibration. */
    cal.grid_w=BLACK_GRID_W; cal.grid_h=BLACK_GRID_H;
    char path[]="/tmp/rear-dark-loader.XXXXXX";
    int temp=mkstemp(path); assert(temp>=0); close(temp);
    FILE *cp=fopen(path,"wb"); assert(cp);
    assert(fwrite("RWBLK001",1,8,cp)==8);
    assert(fwrite(&cal.width,4,10,cp)==10);
    for(unsigned k=0;k<2;k++) {
        assert(fwrite(&cal.profile[k].gain,4,1,cp)==1);
        assert(fwrite(cal.profile[k].grid,sizeof(cal.profile[k].grid),1,cp)==1);
    }
    assert(fclose(cp)==0);
    struct isp_black_calibration loaded;
    assert(isp_load_black_calibration(&loaded,path) && loaded.profiles==2);
    cp=fopen(path,"ab"); assert(cp); fputc(0,cp); fclose(cp);
    assert(!isp_load_black_calibration(&loaded,path) && loaded.profiles==0);
    cp=fopen(path,"wb"); assert(cp); fwrite("RWBLK001",1,8,cp); fclose(cp);
    assert(!isp_load_black_calibration(&loaded,path)); unlink(path);

    /* Two capture/render states may coexist; no process-global pedestal remains. */
    struct thread_job jobs[2]={{.snapshot=snapshot,.raw=banded},{.snapshot=reader,.raw=banded}};
    pthread_t threads[2];
    for(unsigned i=0;i<2;i++) { render(&jobs[i].snapshot,banded,jobs[i].expected); assert(pthread_create(&threads[i],NULL,draw_thread,&jobs[i])==0); }
    for(unsigned i=0;i<100;i++) { reader.black.site[0]=(int)i; render(&reader,banded,out); }
    for(unsigned i=0;i<2;i++) assert(pthread_join(threads[i],NULL)==0);
    puts("frame-owned black correction: preview, cache, stats, still and concurrent render PASS");
    return 0;
}
