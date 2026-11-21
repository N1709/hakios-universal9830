#!/usr/bin/env python3
"""Report S5K2LD control/timing differences in local Samsung and port source.

This inspects the known source shapes. It is not a C preprocessor or an on-device
readback. Unknown shapes fail instead of silently reporting a passing audit.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent


def constant(source, name):
    match = re.search(r'^\s*#define\s+' + re.escape(name) + r'\s+\(?\s*(0x[0-9a-fA-F]+|[0-9]+)\s*\)?', source, re.M)
    if match is None:
        raise ValueError(f'missing numeric constant {name}')
    return int(match[1], 0)


def audit(repo, cis_directory):
    stock_h = (cis_directory / 'is-cis-2ld.h').read_text()
    stock_c = (cis_directory / 'is-cis-2ld.c').read_text()
    set_a = (cis_directory / 'is-cis-2ld-setA.h').read_text()
    port = (repo / 'xnu/files/bsd/dev/redwood_camera.c').read_text()
    receiver = (repo / 'xnu/files/bsd/dev/redwood_camera_s20.c').read_text()
    # Manual findings below were reviewed at these port source hashes. Refuse to
    # repeat them against changed code without a new source review.
    reviewed = {"port_camera": "27febc3b00a91ec9d70b8555a1ab248fbb5d7cae3fa9a9534e3a3fe958a8416d",
                "port_receiver": "c6fd5a1a39308f58e5b41bc0145dfc9b11ac9e2d583dbd4ab77ce890c85e0d3f"}
    for name, text in (("port_camera", port), ("port_receiver", receiver)):
        if hashlib.sha256(text.encode()).hexdigest() != reviewed[name]:
            raise ValueError('port source changed: manual findings require renewed review')
    results = []
    for width, height in ((2016, 1512), (4032, 3024)):
        pll_name = f'sensor_2ld_pllinfo_A_{width}x{height}_30fps'
        match = re.search(re.escape(pll_name) + r'\s*=\s*\{(.*?)\};', set_a, re.S)
        if match is None:
            raise ValueError(f'missing PLL definition {pll_name}')
        pll = re.sub(r'/\*.*?\*/|//[^\n]*', '', match[1], flags=re.S)
        fields = [v.strip() for v in pll.split(',') if v.strip()]
        clock, frame, line = (int(v, 0) for v in fields[2:5])
        match = re.search(r'\{\s*' + str(width) + r'\s*,\s*' + str(height) + r'\s*,([^{}]+)\}', port)
        if match is None:
            raise ValueError('missing built-in rear mode')
        fields = [v.strip() for v in match[1].split(',')]
        # Remaining fields: x,y,lanes,cphy,settle,pclk,line,frame,fps,datatype.
        lanes, cphy, settle, port_clock, port_line, port_frame, fps = map(int, fields[2:9])
        if (port_line, port_frame) != (line, frame):
            raise ValueError('port sensor table dimensions/timing require a new audit')
        results.append(dict(mode=f'{width}x{height}', stock_pclk=clock, port_pclk=port_clock,
            stock_line_ns_floor=line * 10**9 // clock,
            port_line_ns_nominal=10**12 // (fps * frame),
            stock_fps_from_pll=clock / (line * frame), nominal_port_fps=fps / 1000,
            descriptor=dict(lanes=lanes, cphy=cphy, settle=settle),
            notes=['rear receiver uses a separate C-PHY path; descriptor mismatch alone does not prove link failure']))
    min_coarse = constant(stock_h, 'SENSOR_2LD_COARSE_INTEGRATION_TIME_MIN')
    minimum_found = re.search(r'\*lines < \(s20_real \? (\d+)u', port)
    if minimum_found is None:
        raise ValueError('unrecognized port exposure minimum expression')
    hold_write = 'cam_3j1_write8(&sensor_bus, 0x0104, 1)' in receiver
    if 'remainder_cit = short_coarse_int % 2;' not in stock_c:
        raise ValueError('stock LNOFF exposure quantum no longer matches researched source')
    return dict(source_hashes={name: hashlib.sha256(text.encode()).hexdigest() for name, text in
                [('stock_header', stock_h), ('stock_driver', stock_c), ('stock_setA', set_a),
                 ('port_camera', port), ('port_receiver', receiver)]}, modes=results,
        controls=dict(stock_USE_GROUP_PARAM_HOLD=constant(stock_h, 'USE_GROUP_PARAM_HOLD'),
                      port_rear_group_hold_write_present=hold_write,
                      stock_min_coarse=min_coarse, port_s20_min_coarse=int(minimum_found[1]),
                      stock_normal_exposure_quantum=2,
                      stock_LN2_exposure_quantum=4, stock_LN4_exposure_quantum=8,
                      stock_normal_margin=constant(stock_h, 'SENSOR_2LD_COARSE_INTEGRATION_TIME_MAX_MARGIN'),
                      port_margin=constant(port, 'EXPOSURE_MARGIN')),
        manual_findings=['port exposure_limits has no rear even-line quantization',
                         'rear runtime exposure I2C return values are discarded',
                         'full-mode stock LN2 requires 72-line margin; port uses 40 and leaves LN2 off',
                         'stock binned duration/exposure shifting starts above 80000 us; port has no equivalent control path'],
        boundary='Source comparison only; verify exact shipping kernel and register readbacks before hardware changes')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('cis_directory', type=Path)
    parser.add_argument('--repo', type=Path, default=HERE.parents[1])
    args = parser.parse_args()
    try:
        result = audit(args.repo, args.cis_directory)
    except (OSError, ValueError, IndexError) as error:
        parser.exit(1, f'control audit: {error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
