#!/usr/bin/env python3
"""Prepare isolated original/experimental workers; fail if their shared ABI differs.

The device, stream start/stop, cross-camera exclusion and object ownership remain
in the original implementation. Only worker_body dispatches, once per stream.
The two ISP implementations have separate symbols and black-state storage.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
ORIGINAL = 'd5b02944ec77e3d63dd550db8af0780a42d8b973'


def original(name):
    return subprocess.check_output(['git', 'show', f'{ORIGINAL}:userland/src/{name}'], cwd=ROOT).decode()


def declaration(source, name):
    # Compare complete shared object declarations, with whitespace/comments removed.
    match = re.search(r'struct\s+'+name+r'\s*\{', source)
    if not match:
        raise ValueError('missing shared declaration '+name)
    a = match.start()
    depth, b = 1, match.end()
    while depth:
        depth += (source[b] == '{') - (source[b] == '}')
        b += 1
    text = re.sub(r'/\*.*?\*/|//[^\n]*', '', source[a:b], flags=re.S)
    return re.sub(r'\s+', '', text)


def primary_stats_source():
    source=original('camera_isp.c')
    start=source.index('isp_measure(')
    end=source.index('\nbool\nisp_ae(',start)
    body=source[start:end]
    # Only rear-main binned stats: 12k sample cells instead of48k. Pixel
    # rendering, front and UW sampling are identical to the pinned source.
    body=body.replace('for (uint32_t cy = 0; cy < cells_h; cy += 4)',
        'const unsigned step=f->sensor==1 ? (f->width==4032 ? 16 : f->width==2016 ? 8 : 4) : 4;\n\tfor (uint32_t cy = 0; cy < cells_h; cy += step)')
    body=body.replace('cx += 4)', 'cx += step)')
    if 'const unsigned step=' not in body:raise ValueError('primary stats loop changed')
    return source[:start]+body+source[end:]


def primary_af_source(source):
    # The kernel converts userland0..1022 to the AK737x motor0..511.
    # Keep this scaled ABI; a511 cap would prevent reaching close focus.
    source=source.replace('struct af {\n','struct af {\n\tuint32_t primary_limit, primary_watch;\n',1)
    a=source.index('static void\naf_go(')
    b=source.index("/* the sensor's colour tuning",a)
    body=source[a:b].replace('AF_FAR','af_limit(af)').replace('AF_NEAR','af_minimum(af)')
    for op in ('>','<=','<','>='):
        body=body.replace('next '+op+' af_limit(af)','next '+op+' (int)af_limit(af)')
        body=body.replace('next '+op+' af_minimum(af)','next '+op+' (int)af_minimum(af)')
        body=body.replace('af->way * AF_STEP '+op+' af_limit(af)','af->way * AF_STEP '+op+' (int)af_limit(af)')
        body=body.replace('af->way * AF_STEP '+op+' af_minimum(af)','af->way * AF_STEP '+op+' (int)af_minimum(af)')
    body=body.replace('unsigned needed = f->gain >= 16 * f->gain_unity ? 2 : 1;',
        'unsigned needed = f->sensor==1 ? 1 : f->gain >= 16 * f->gain_unity ? 2 : 1;')
    begin=body.index('if (f->focus == REDWOOD_CAMERA_NO_FOCUS)')
    body=body[:begin]+'if(f->sensor==1){\n        af->primary_limit=1022;\n        if(locked && f->focus!=REDWOOD_CAMERA_NO_FOCUS){af->state=AF_WATCH;af->at=af->target=f->focus;af->wait=0;af->now=af->sweep=false;return;}\n        if(!locked && af->state==AF_WATCH && af->at==af->target && (++af->primary_watch&1))return;\n    }\n    '+body[begin:]
    body=body.replace('++af->unlike < 30','++af->unlike < (f->sensor==1?3:30)')
    body=body.replace('af->steady < 10','af->steady < (f->sensor==1?3:10)')
    marker='if (af->worse < 2 && (int)af->at + af->way * AF_STEP'
    assert marker in body
    body=body.replace(marker,'if(f->sensor==1){\n            int next=(int)af->at+af->way*AF_STEP;\n            next=next<(int)af_minimum(af)?(int)af_minimum(af):next>(int)af_limit(af)?(int)af_limit(af):next;\n            if((uint32_t)next!=af->at){af_go(device,af,(uint32_t)next);return;}\n        }\n        if (f->sensor!=1 && af->worse < 2 && (int)af->at + af->way * AF_STEP')
    helpers='static uint32_t af_limit(const struct af *af){return af->primary_limit?af->primary_limit:AF_FAR;}\n'
    helpers+='static uint32_t af_minimum(const struct af *af){return af->primary_limit?0:AF_NEAR;}\n\n'
    body=body.replace('f->sensor==1','(f->sensor==1||f->sensor==4)').replace('f->sensor!=1','(f->sensor!=1&&f->sensor!=4)')
    return source[:a]+helpers+body+source[b:]


def prepare(out):
    source = primary_af_source(original('camera_device.c'))
    source = source.replace('#include "camera_isp.h"', '#include "camera_isp.h"\n#include "camera_s20_zoom.h"')
    source = source.replace('{ "MaxZoomFactor", "Number", "ReadOnly" }', '{ "MinZoomFactor", "Number", "ReadOnly" }, { "MaxZoomFactor", "Number", "ReadOnly" }')
    source = source.replace('} else if (key_is(key, "MaxZoomFactor")) {', '} else if (key_is(key, "MinZoomFactor")) {\n        value=number_f(s->camera->sensor==1&&access(rw_path("redwood-ultrawide-enable",(char[160]){0},160),F_OK)==0?0.5f:1.0f);\n\t} else if (key_is(key, "MaxZoomFactor")) {')
    source = source.replace('zoom = zoom < 1 ? 1 : zoom > MAX_ZOOM ? MAX_ZOOM : zoom;', 'zoom=s20_zoom_clamp((float)zoom,s->camera->sensor==1,access(rw_path("redwood-ultrawide-enable",(char[160]){0},160),F_OK)==0);')
    # Correctly powered primary dark frames are64..66 at1x..16x. Legacy
    # undervoltage compensations are now opt-in, never a fresh-build default.
    source=source.replace('while (s->running && failures < 5) {',
        'bool primary_legacy_black=s->camera->sensor==1&&access(rw_path("redwood-primary-legacy-black",(char[160]){0},160),F_OK)==0;\n\twhile (s->running && failures < 5) {')
    source=source.replace('if (fw->width == 2016 && fw->gain_unity > 0)',
        'if (fw->sensor==1 && fw->width == 2016 && fw->gain_unity > 0 && primary_legacy_black)')
    source=source.replace('if (fw->width == 2016 || fw->width == 4032)',
        'if ((fw->width == 2016 || fw->width == 4032) && !(fw->sensor==1 && !primary_legacy_black))')
    source=source.replace('if (fb->width == 4032) {',
        'if (fb->width == 4032 && primary_legacy_black) {')
    source=source.replace('if (fd->width == 4032 && fd->gain_unity > 0)',
        'if (fd->width == 4032 && fd->gain_unity > 0 && primary_legacy_black)')
    source=source.replace('.b = want_rear ? 2 :',
        '.b = want_rear ? (access(rw_path("redwood-primary-full",(char[160]){0},160),F_OK)==0?3:2) :')
    source=source.replace('FILE *cf = fopen(rw_path("redwood-contrast", (char[160]){0}, 160), "r");',
        'FILE *cf=s->camera->sensor==1?fopen(rw_path("redwood-primary-contrast",(char[160]){0},160),"r"):NULL;\n\t\tif(!cf)cf=fopen(rw_path("redwood-contrast",(char[160]){0},160),"r");')
    current = primary_af_source((ROOT/'userland/src/camera_device.c').read_text())
    shared = {}
    for name in ('stream', 'output', 'camera', 'device'):
        a, b = declaration(source, name), declaration(current, name)
        if a != b:
            raise ValueError('shared ABI changed: '+name+'; review bridge before building')
        shared[name] = hashlib.sha256(a.encode()).hexdigest()
    bridge = r'''
extern void *redwood_camera_experimental_worker(void *context);
static bool front_experimental_profile(void)
{
    char value[32] = {0};
    FILE *fp = fopen(rw_path("redwood-front-profile", (char[160]){0}, 160), "r");
    if (!fp) return false;
    bool enabled = fscanf(fp, "%31s", value) == 1 && strcmp(value, "experimental") == 0;
    fclose(fp); return enabled;
}
static float rear_requested_zoom(struct stream *s)
{
    float z=s->zoom;FILE *f=fopen(rw_path("redwood-rear-zoom",(char[160]){0},160),"r");
    if(f){float v;if(fscanf(f,"%f",&v)==1)z=v;fclose(f);}
    return s20_zoom_clamp(z,true,true);
}
static bool rear_ultra_selected(struct stream *s)
{
    if(s->camera->sensor!=1 || access(rw_path("redwood-ultrawide-enable",(char[160]){0},160),F_OK)!=0)return false;
    return s20_zoom_mode_tele(rear_requested_zoom(s),true,access(rw_path("redwood-gw2-enable",(char[160]){0},160),F_OK)==0)!=2;
}
static void *worker_original_body(void *context);
static void *worker_body(void *context)
{
    struct stream *s=stream_of(context);
    if(s->camera->sensor!=1)return front_experimental_profile()?redwood_camera_experimental_worker(context):worker_original_body(context);
    while(s->running){
        bool ultra=rear_ultra_selected(s);
        say("profile: rear %s worker",ultra?"ultra-wide":"original primary");
        if(ultra)redwood_camera_experimental_worker(context);else worker_original_body(context);
        if(!s->running || ultra==rear_ultra_selected(s))break;
    }
    return NULL;
}
'''
    marker = 'static void *\nworker_body(void *context)\n{'
    if source.count(marker) != 1:
        raise ValueError('original worker changed')
    source = source.replace(marker, bridge+'\nstatic void *\nworker_original_body(void *context)\n{')
    source = source.replace('while (s->running && failures < 5) {', 'while (s->running && failures < 5) {\n        if(rear_ultra_selected(s))break;')
    # Report the actual primary lock state; other sensors retain the pinned metadata.
    source=source.replace('const struct isp_stats *st, CFStringRef port)',
        'const struct isp_stats *st, CFStringRef port, bool af_locked, bool ae_locked)')
    source=source.replace('frame_metadata(&isp, f, &st, s->port)',
        'frame_metadata(&isp, f, &st, s->port, f->sensor==1&&s->af_locked, f->sensor==1&&s->ae_locked)')
    source=source.replace('md_put(m, MD_AF_LOCKED, CFRetain(kCFBooleanFalse));',
        'md_put(m, MD_AF_LOCKED, CFRetain(af_locked?kCFBooleanTrue:kCFBooleanFalse));')
    source=source.replace('md_put(m, MD_AE_LOCKED, CFRetain(kCFBooleanFalse));',
        'md_put(m, MD_AE_LOCKED, CFRetain(ae_locked?kCFBooleanTrue:kCFBooleanFalse));')
    # Keep the iOS graph fixed; our native rear control owns the physical route
    # and digital crop. This avoids graph device reconfiguration while recording.
    source=source.replace('isp.zoom = s->zoom;',
        'isp.zoom=s->camera->sensor==1?rear_requested_zoom(s):s->zoom;')
    source = source.replace('if (n != (ssize_t)sizeof(head)) {', 'if(n==(ssize_t)sizeof(head)&&s->camera->sensor==1&&(head.sensor==3||head.sensor==4))continue;\n            if (n != (ssize_t)sizeof(head)) {')

    a=source.index('static void *\nworker_original_body(void *context)\n{')
    b=source.index('/*\n * Reading frames from the kernel',a)
    body=source[a:b].replace('s->af_window','measure_window')
    body=body.replace('af_frame(device, &af, f, raw, measure_window, s->af_locked);','af_frame(device, &af, f, raw, focus_window, s->af_locked);')
    raw='const uint8_t *raw = buffer + f->header_bytes;'
    assert raw in body
    body=body.replace(raw,raw+'\n        float measure_window[4];\n        memcpy(measure_window,s->af_window,sizeof(measure_window));\n        if(f->sensor==1)s20_zoom_window(s->af_window,measure_window,rear_requested_zoom(s));\n        float focus_window[4];memcpy(focus_window,measure_window,sizeof(focus_window));\n        if(f->sensor==1){for(unsigned axis=0;axis<2;axis++){float centre=(measure_window[axis]+measure_window[axis+2])*.5f;float half=(measure_window[axis+2]-measure_window[axis])*.25f;focus_window[axis]=centre-half;focus_window[axis+2]=centre+half;}}')
    source=source[:a]+body+source[b:]
    current = current.replace('while (s->running && failures < 5) {', 'while (s->running && failures < 5) {\n        if(s->camera->sensor==1){float z=s->zoom;FILE *lf=fopen(rw_path("redwood-rear-zoom",(char[160]){0},160),"r");if(lf){float v;if(fscanf(lf,"%f",&v)==1)z=v;fclose(lf);}if(s20_zoom_mode_tele(s20_zoom_clamp(z,true,true),true,access(rw_path("redwood-gw2-enable",(char[160]){0},160),F_OK)==0)==2)break;}')
    out.mkdir(parents=True, exist_ok=True)
    legacy = out/'original'
    legacy.mkdir(exist_ok=True)
    (legacy/'camera_device.c').write_text(source)
    for name in ('camera_isp.c', 'camera_isp.h', 'redwood_camera.h'):
        (legacy/name).write_text(primary_stats_source() if name=='camera_isp.c' else original(name))
    header = (ROOT/'userland/src/camera_isp.h').read_text()
    names = set(re.findall(r'\b(isp_\w+)\s*\(', header))
    names |= {'redwood_camera_device_create', 'redwood_camera_say'}
    (out/'experimental-symbols.h').write_text(''.join(f'#define {n} experimental_{n}\n' for n in sorted(names)))
    current=current.replace('const struct isp_stats *st, CFStringRef port)',
        'const struct isp_stats *st, CFStringRef port, bool af_locked, bool ae_locked)')
    current=current.replace('frame_metadata(&isp, f, &st, s->port)',
        'frame_metadata(&isp, f, &st, s->port, f->sensor==4&&s->af_locked, f->sensor==4&&s->ae_locked)')
    current=current.replace('md_put(m, MD_AF_LOCKED, CFRetain(kCFBooleanFalse));',
        'md_put(m, MD_AF_LOCKED, CFRetain(af_locked?kCFBooleanTrue:kCFBooleanFalse));')
    current=current.replace('md_put(m, MD_AE_LOCKED, CFRetain(kCFBooleanFalse));',
        'md_put(m, MD_AE_LOCKED, CFRetain(ae_locked?kCFBooleanTrue:kCFBooleanFalse));')
    current=current.replace('af_frame(device, &af, f, raw, s->af_window, s->af_locked);',
        'float tele_window[4];memcpy(tele_window,s->af_window,sizeof(tele_window));if(f->sensor==4)s20_zoom_window(s->af_window,tele_window,s20_zoom_crop_sensor(requested_zoom,4));\n        af_frame(device, &af, f, raw, tele_window, s->af_locked);')
    current += r'''
/* Worker boundary only: original device/stream lifecycle owns exclusion. */
static pthread_once_t experimental_functions_once = PTHREAD_ONCE_INIT;
static bool experimental_functions_ok;
static void experimental_find_functions(void) { experimental_functions_ok = find_functions(); }
void *redwood_camera_experimental_worker(void *context);
void *redwood_camera_experimental_worker(void *context)
{
    pthread_once(&experimental_functions_once, experimental_find_functions);
    return experimental_functions_ok ? worker_body(context) : NULL;
}
'''
    (out/'experimental-device.c').write_text(current)
    report = dict(original_source_commit=ORIGINAL, shared_layout_hashes=shared,
                  default_front='original', rear='original primary; experimental ultra-wide',
                  profile_flag='redwood-front-profile', selection='per stream; no mid-frame switch',
                  warning='experimental profile is not verified Samsung factory tuning')
    (out/'profile-build.json').write_text(json.dumps(report, indent=2)+'\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    print(json.dumps(prepare(args.output), indent=2))
