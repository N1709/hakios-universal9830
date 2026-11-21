#!/usr/bin/env python3
"""Compile real kernel normalization and I/O helper with host test doubles."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'xnu/files/bsd/dev/redwood_camera.c').read_text()
start=source.index('static void\nexposure_limits(')
brace=source.index('{',start)
depth=1
end=brace+1
while depth:
    depth += (source[end]=='{') - (source[end]=='}')
    end += 1
function=source[start:end]
# Preserve the actual production constants; stub only other-device state.
def constant(name):
    match=re.search(r'^#define\s+'+name+r'\s+(\w+)',source,re.M)
    if not match:
        raise RuntimeError('missing production constant '+name)
    return match[1]
prefix='''#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "redwood_camera_s20_controls.h"
static bool s20_real,s20_rear_now;
static struct {uint32_t frame_length;} mode;
static const typeof(mode) *ios_mode=&mode;
#define GAIN_UNITY 32
#define GAIN_MAX 512
'''
prefix += '#define FRAME_LINES_MAX '+constant('FRAME_LINES_MAX')+'\n'
prefix += '#define EXPOSURE_MARGIN '+constant('EXPOSURE_MARGIN')+'\n'
with tempfile.TemporaryDirectory(prefix='s20-controls-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(prefix+function+'\n'+(ROOT/'tools/camera/test_s20_controls.c').read_text())
    subprocess.run([os.environ.get('CC','clang'),'-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-I'+str(ROOT/'xnu/files/bsd/dev'),
                    str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
