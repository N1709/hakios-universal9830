#!/usr/bin/env python3
"""Prepare an S20 memory-layout test from the phone's own recovery backup.

Offline only. Preserve installed BootShim, DT table, kernel, root and payload
arguments; replace the 2-MiB FD and loader, then rebuild Android/AVB hashes.
For allocator preflight, keep the installed FD and retain its original loader
as fallback.efi. No native firmware replacement is needed for that probe.
Produces a verified candidate, bounded write segments, rollback segments and
a metadata manifest. Does not contact or write to the phone.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
RESEARCH = ROOT.parent.parent
PART_SIZE = 69009408
FD_OFFSET = 0x870
FD_SIZE = 0x200000
SLOT = FD_OFFSET + 0x100000
SLOT_SIZE = 0x80000
HEAD_SIZE = 0x1000000


def sha(data):
    return hashlib.sha256(data).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def entries(image):
    require(image[SLOT:SLOT + 4] == b'XNUP', 'missing installed payload')
    body_size, = struct.unpack_from('<I', image, SLOT + 4)
    require(body_size <= SLOT_SIZE - 8, 'payload exceeds slot')
    cursor, end = SLOT + 8, SLOT + 8 + body_size
    result, names = [], set()
    while cursor + 24 <= end:
        name, size, reserved = struct.unpack_from('<16sII', image, cursor)
        if not any(name):
            return result
        raw = name.rstrip(b'\0')
        require(b'\0' not in raw and raw not in names, 'invalid/duplicate payload name')
        require(cursor + 24 + ((size + 15) & ~15) <= end, 'truncated payload item')
        names.add(raw)
        result.append((name, reserved, image[cursor + 24:cursor + 24 + size]))
        cursor += 24 + ((size + 15) & ~15)
    raise ValueError('missing payload terminator')


def boot_layout(image):
    require(len(image) == PART_SIZE and image[:8] == b'ANDROID!', 'not a full S20 recovery backup')
    page, version = struct.unpack_from('<II', image, 36)
    require(page == 2048 and version == 2, 'expected page-2048 Android boot v2')
    sizes = [struct.unpack_from('<I', image, offset)[0] for offset in (8, 16, 24, 1632, 1648)]
    require(sizes[0] == FD_SIZE + 0x70, 'unexpected BootShim/FD kernel layout')
    spans, cursor = [], page
    for size in sizes:
        spans.append((cursor, size))
        cursor += (size + page - 1) // page * page
    require(cursor <= HEAD_SIZE and image[spans[-1][0]:spans[-1][0] + 4] == bytes.fromhex('d7b7ab1e'),
            'missing Samsung DT table or boot image exceeds head')
    return spans, (cursor + 4095) & ~4095


def verify(avb, key, image):
    # avbtool resolves the recovery hash descriptor by basename recovery.img.
    with tempfile.TemporaryDirectory(dir=image.parent) as directory:
        link = Path(directory) / 'recovery.img'
        link.symlink_to(image.resolve())
        subprocess.run(['python3', '-I', str(avb), 'verify_image', '--image', str(link),
                        '--key', str(key)], check=True, timeout=30)


def replace_loader(old_entries, loader, with_fallback=False):
    require(sum(name.rstrip(b'\0') == b'loader.efi' for name, _, _ in old_entries) == 1,
            'expected exactly one installed loader')
    require(not any(name.rstrip(b'\0') in (b'fallback.efi', b'probe.report') for name, _, _ in old_entries),
            'backup already contains a fallback; use the original phone backup')
    preflight = any(marker in loader for marker in
                    (b'CELLULAR MEMORY PREFLIGHT', b'CELLULAR CP BOOT PREFLIGHT'))
    require(preflight == with_fallback, 'preflight requires the installed fallback; normal loader must not use it')
    replaced = []
    for name, reserved, data in old_entries:
        if name.rstrip(b'\0') == b'loader.efi':
            replaced.append((name, reserved, loader))
            if with_fallback:
                replaced.append((b'fallback.efi'.ljust(16, b'\0'), 0, data))
                replaced.append((b'probe.report'.ljust(16, b'\0'), 0, bytes(16384)))
        else:
            replaced.append((name, reserved, data))
    return replaced


def cp_boot_payload(radio):
    """Explicit BOOT-only extraction; no NV/EFS or MAIN payload reads."""
    spec = importlib.util.spec_from_file_location('cellular_shannon', Path(__file__).with_name('shannon.py'))
    shannon = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(shannon)
    with radio.open('rb') as stream:
        records = shannon.read_toc(stream, radio.stat().st_size, False)
        boot = next(record for record in records if record['name'] == 'BOOT')
        stream.seek(boot['file_offset'])
        data = stream.read(boot['size'])
    require(len(data) == boot['size'] and zlib.crc32(data) == boot['crc32'], 'BOOT CRC/length mismatch')
    info = struct.pack('<4sIIIII', b'S512', 1, len(data), boot['crc32'], boot['cp_address'], boot['entry_id'])
    return data, info, dict(size=len(data), crc32=hex(boot['crc32']), sha256=sha(data),
                            cp_address=hex(boot['cp_address']), entry_id=boot['entry_id'],
                            scope='BOOT only; no NV/EFS or subscriber data')


def replace_kernel(image, kernel, records):
    """Replace only the fixed 16-MiB AP-kernel segment, preserving root/logs."""
    values = {name.rstrip(b'\0'): data.rstrip(b'\0') for name, _, data in records}
    require(values.get(b'kpart') == b'recovery' and values.get(b'koff') == b'2000000' and
            values.get(b'rpart') == b'recovery' and values.get(b'roff') == b'3400000',
            'kernel replacement requires the known recovery kernel/root layout')
    require(4096 <= len(kernel) <= 0x1000000 and
            struct.unpack_from('<II', kernel) == (0xfeedfacf, 0x0100000c),
            'new kernel must be ARM64 Mach-O and fit the 16-MiB staging window')
    require(len(image) >= 0x3400000 and image[0x2000000:0x2000008] == kernel[:8],
            'original recovery does not contain the expected ARM64 kernel')
    image[0x2000000:0x3000000] = kernel.ljust(0x1000000, b'\0')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backup', type=Path, required=True)
    firmware = parser.add_mutually_exclusive_group(required=True)
    firmware.add_argument('--fd', type=Path)
    firmware.add_argument('--keep-installed-fd', action='store_true')
    parser.add_argument('--with-installed-fallback', action='store_true')
    parser.add_argument('--cp-boot-radio', type=Path,
                        help='explicit local BOOT-only extraction for a CP BOOT preflight; never NV')
    parser.add_argument('--loader', type=Path, required=True)
    parser.add_argument('--kernel', type=Path,
                        help='optional new AP kernel; fixed16-MiB recovery segment only, root/logs preserved')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--avbtool', type=Path, default=RESEARCH / 'tools/bin/avbtool.py')
    parser.add_argument('--key', type=Path, default=RESEARCH / 'tools/dl/avb/testkey_rsa4096.pem')
    args = parser.parse_args()
    original = args.backup.read_bytes()
    spans, boot_size = boot_layout(original)
    verify(args.avbtool, args.key, args.backup)
    old_entries = entries(original)
    require(not args.with_installed_fallback or args.keep_installed_fd,
            'preflight must keep the installed firmware')
    fd = (original[FD_OFFSET:FD_OFFSET + FD_SIZE] if args.keep_installed_fd else args.fd.read_bytes())
    fd = bytearray(fd)
    if args.keep_installed_fd:
        fd[0x100000:0x180000] = b'\xff' * SLOT_SIZE
    loader = args.loader.read_bytes()
    require(len(fd) == FD_SIZE and fd[0x28:0x2c] == b'_FVH', 'not the expected 2-MiB FD')
    require(fd[0x100000:0x180000] == b'\xff' * SLOT_SIZE, 'new FD occupies payload slot')
    require(loader[:2] == b'MZ' and len(loader) >= 64, 'loader is not PE/COFF')
    pe, = struct.unpack_from('<I', loader, 0x3c)
    require(loader[pe:pe + 4] == b'PE\0\0' and struct.unpack_from('<H', loader, pe + 4)[0] == 0xaa64,
            'loader is not ARM64 PE/COFF')
    require(struct.unpack_from('<H', loader, pe + 24 + 68)[0] == 10, 'loader is not an EFI application')
    replaced = replace_loader(old_entries, loader, args.with_installed_fallback)
    cp_metadata = None
    cp_probe = b'cellular CP BOOT:' in loader
    require(cp_probe == bool(args.cp_boot_radio), 'CP BOOT preflight requires exactly one explicit radio source')
    if args.cp_boot_radio:
        require(args.keep_installed_fd and args.with_installed_fallback,
                'CP BOOT probe must preserve original firmware and fallback')
        require(not any(name.rstrip(b'\0') in (b'cp.boot', b'cp.boot.info') for name, _, _ in old_entries),
                'backup already contains CP BOOT entries; use the original phone backup')
        cp_data, cp_info, cp_metadata = cp_boot_payload(args.cp_boot_radio)
        replaced += [(b'cp.boot'.ljust(16, b'\0'), 0, cp_data),
                     (b'cp.boot.info'.ljust(16, b'\0'), 0, cp_info)]
    body = b''.join(struct.pack('<16sII', name, len(data), reserved) + data + bytes(-len(data) % 16)
                    for name, reserved, data in replaced) + bytes(24)
    payload = b'XNUP' + struct.pack('<I', len(body)) + body
    require(len(payload) <= SLOT_SIZE, 'replacement payload too large')
    changed = bytearray(original)
    if args.kernel:
        require(args.keep_installed_fd and not args.with_installed_fallback,
                'AP kernel replacement requires original firmware and a normal loader')
        replace_kernel(changed, args.kernel.read_bytes(), old_entries)
    changed[FD_OFFSET:FD_OFFSET + FD_SIZE] = fd
    changed[SLOT:SLOT + len(payload)] = payload
    require(entries(changed) == replaced, 'replacement payload did not round-trip')
    digest = hashlib.sha1()
    for offset, size in spans:
        digest.update(changed[offset:offset + size])
        digest.update(struct.pack('<I', size))
    changed[576:608] = digest.digest().ljust(32, b'\0')
    output = args.output
    output.mkdir(parents=True, exist_ok=False)
    head = output / 'signed-head.img'
    head.write_bytes(changed[:boot_size])
    subprocess.run(['python3', '-I', str(args.avbtool), 'add_hash_footer', '--image', str(head),
                    '--partition_size', str(PART_SIZE), '--partition_name', 'recovery',
                    '--algorithm', 'SHA256_RSA4096', '--key', str(args.key), '--rollback_index', '1'],
                   check=True, timeout=30)
    signed = head.read_bytes()
    # AVB's footer must point into the writeable head; kernel/root stay intact.
    require(signed[-64:-60] == b'AVBf', 'missing new AVB footer')
    _, _, original_size, vb_offset, vb_size = struct.unpack_from('>IIQQQ', signed, PART_SIZE - 60)
    require(original_size == boot_size and vb_offset + vb_size <= HEAD_SIZE,
            'unexpected AVB metadata layout')
    changed[:HEAD_SIZE] = signed[:HEAD_SIZE]
    changed[-64:] = signed[-64:]
    if args.kernel:
        require(changed[HEAD_SIZE:0x2000000] == original[HEAD_SIZE:0x2000000] and
                changed[0x3000000:-64] == original[0x3000000:-64],
                'root/log/body outside the AP kernel segment changed')
    else:
        require(changed[HEAD_SIZE:-64] == original[HEAD_SIZE:-64], 'kernel/root/log body changed')
    require(changed[2048:FD_OFFSET] == original[2048:FD_OFFSET], 'installed BootShim changed')
    if args.keep_installed_fd:
        require(changed[FD_OFFSET:SLOT] == original[FD_OFFSET:SLOT] and
                changed[SLOT + SLOT_SIZE:FD_OFFSET + FD_SIZE] == original[SLOT + SLOT_SIZE:FD_OFFSET + FD_SIZE],
                'installed firmware code changed')
    require(entries(changed) == replaced, 'signed payload changed')
    candidate = output / 'recovery.img'
    candidate.write_bytes(changed)
    verify(args.avbtool, args.key, candidate)
    (output / 'head.bin').write_bytes(changed[:HEAD_SIZE])
    (output / 'rollback-head.bin').write_bytes(original[:HEAD_SIZE])
    (output / 'standalone-boot-head.bin').write_bytes(changed[:boot_size])
    if args.kernel:
        (output / 'kernel-segment.bin').write_bytes(changed[0x2000000:0x3000000])
        (output / 'rollback-kernel-segment.bin').write_bytes(original[0x2000000:0x3000000])
    footer_changed = changed[-64:] != original[-64:]
    if footer_changed:
        (output / 'footer-block.bin').write_bytes(changed[-4096:])
        (output / 'rollback-footer-block.bin').write_bytes(original[-4096:])
    manifest = dict(scope='offline candidate only; no phone writes',
                    backup_sha256=sha(original), candidate_sha256=sha(changed),
                    fd_sha256=sha(fd), loader_sha256=sha(loader),
                    partition_size=PART_SIZE, head_size=HEAD_SIZE,
                    standalone_boot_size=boot_size,
                    standalone_boot_sha256=sha(changed[:boot_size]),
                    original_boot_prefix_sha256=sha(original[:boot_size]),
                    footer_changed=footer_changed, footer_block_offset=PART_SIZE - 4096,
                    kernel_root_log_body_preserved=not bool(args.kernel), bootshim_preserved=True,
                    root_body_preserved=True, ap_kernel_changed=bool(args.kernel),
                    payload_arguments_preserved=True, avb_verified=True)
    manifest.update(installed_firmware_preserved=args.keep_installed_fd,
                    installed_loader_fallback=args.with_installed_fallback)
    if args.kernel:
        manifest.update(kernel_segment_offset=0x2000000, kernel_segment_size=0x1000000,
                        kernel_segment_sha256=sha(changed[0x2000000:0x3000000]),
                        original_kernel_segment_sha256=sha(original[0x2000000:0x3000000]))
    if cp_metadata:
        manifest['cp_boot'] = cp_metadata
    if args.with_installed_fallback:
        cursor = 8
        for name, _, data in replaced:
            if name.rstrip(b'\0') == b'probe.report':
                manifest['preflight_report_pa'] = hex(0xe8100000 + cursor + 24)
                manifest['preflight_report_size'] = len(data)
                break
            cursor += 24 + ((len(data) + 15) & ~15)
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    head.unlink()
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
