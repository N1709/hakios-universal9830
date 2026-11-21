#!/usr/bin/env python3
"""Offline S20 external-SIPC download plan. No device I/O or NV reads. MIT.

Protocol source and limits: docs/devices/x1slte/cellular/download-plan.md. A plan describes
host commands; it does not establish CP compatibility or firmware authenticity.
"""
import argparse
import json
from pathlib import Path
from shannon import read_toc


def build_plan(entries):
    # Deliberately accept only the observed S20 profile, never silently adapt
    # stage numbers from another modem's table or trust the per-entry ID field.
    names = [e['name'] for e in entries]
    if names != ['TOC', 'BOOT', 'MAIN', 'APM', 'VSS', 'NV', 'OFFSET', 'INFO']:
        raise ValueError('unreviewed S20 TOC ordering/profile')
    if entries[0]['entry_id'] != 7:
        raise ValueError('S20 TOC count must include OFFSET and exclude trailing INFO')
    for e in entries:
        if e['name'] in ('BOOT', 'MAIN', 'APM', 'VSS', 'INFO') and not e.get('crc_verified'):
            raise ValueError('firmware CRC verification required')
        if not 0 < e['size'] <= 0xffffffff:
            raise ValueError('invalid image size')
    stages = []
    for stage, e in enumerate([entries[0], *entries[2:6]], 1):
        nv = e['name'] == 'NV'
        stages.append(dict(
            stage=stage, name=e['name'], bytes=e['size'],
            frames=(e['size']+2047)//2048,
            source='external NV; unresolved, never opened' if nv else 'read-only RADIO',
            file_offset=None if nv else e['file_offset'],
            start=dict(request=f'{0x9000 | stage << 8:08x}', ack=f'{0xa000 | stage << 8:08x}'),
            send=dict(command=f'{0x9001 | stage << 8:08x}', ack=f'{0xa001 | stage << 8:08x}',
                      ack_scope='once after all image fragments'),
            crc=dict(request=f'{0x900c | stage << 8:08x}', ack=f'{0xa00c | stage << 8:08x}',
                     value=f"{e['crc32']:08x}") if e['name'] == 'MAIN' else None,
            done=dict(request=f'{0x900d | stage << 8:08x}', ack=f'{0xa00d | stage << 8:08x}'),
            blocked=nv))
    return dict(
        scope='offline source-derived plan; no hardware acceptance or authenticity proof',
        boot='BOOT stage0 already sent over SPI; external loader starts queue stages at TOC1',
        link=dict(channel=215, sipc_header_bytes=4, alignment=8, udl_header_bytes=16,
                  fragment_payload_max=2048, data_numbers='one-based little-endian u32'),
        stages=stages,
        finish=dict(stage=15, request='00009f00', ack='0000af00', blocked=True,
                    reason='requires every earlier stage, NV, and CP completion handshake'),
        excluded=['OFFSET (terminates stage table)', 'INFO (trailing metadata, not a UDL stage)'],
        first_live_transaction='TOC stage1 START only, after IPC mapping/init and DMA prerequisites')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('radio', type=Path)
    args = parser.parse_args()
    try:
        with args.radio.open('rb') as f:
            entries = read_toc(f, args.radio.stat().st_size, verify_crc=True)
        print(json.dumps(build_plan(entries), indent=2))
    except (ValueError, OSError) as e:
        parser.exit(2, f'UDL plan refused: {e}\n')


if __name__ == '__main__':
    main()
