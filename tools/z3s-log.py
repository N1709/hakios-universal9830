#!/usr/bin/env python3
"""Read the z3s loader log that survives an Android boot.

The z3s loader writes its own log, then the kernel log, into
`recovery+16 MiB` (8 MiB window). Android never writes the recovery
partition, so the log is still there after the phone boots Android or TWRP
again. This tool pulls that window over adb (TWRP, or root on Android) or
reads a dump that was copied to the host.

usage:
  tools/z3s-log.py --adb                 # TWRP/root: adb exec-out dd ...
  tools/z3s-log.py --adb --output log.bin
  tools/z3s-log.py --input log.bin
  tools/z3s-log.py --input log.bin --raw > log.raw

The loader refuses to write inside `super` until SUPER_TAIL_START is measured;
with LOG_IN_RECOVERY=1 (the z3s profile) recovery is the log partition.
"""
import argparse
import subprocess
import sys
from pathlib import Path

LOG_PARTITION = "/dev/block/by-name/recovery"
LOG_OFFSET = 0x01000000          # 16 MiB: clear of the UEFI image and the kernel
LOG_LENGTH = 0x00800000          # 8 MiB window (loader log + kernel log)
BLOCK = 4096

LOADER_SEP = b"\n== KERNEL LOG ==\n"
LOADER_END = b"\n== END OF LOG ==\n"


def decode_log(data):
    """Return the text part of the 8 MiB log window.

    The loader zero-fills the rest of the window after `== END OF LOG ==`.
    Some loader builds put a NUL after the text; stop at the first one so the
    output does not end with megabytes of NULs. A window that is all zeroes
    means no run has written a log since the partition was flashed.
    """
    end = data.find(b"\0")
    if end >= 0:
        data = data[:end]
    return data.decode("utf-8", "replace")


def summarize(text):
    if not text.strip():
        return "empty: no log in this window (the loader did not run, or recovery was reflashed)"
    if LOADER_SEP.decode() not in text:
        return "loader log only: the kernel did not return a log"
    if LOADER_END.decode() not in text:
        return "truncated: the kernel log has no end marker"
    return "loader log + kernel log"


def read_adb(serial=None, su=False, partition=LOG_PARTITION,
             offset=LOG_OFFSET, length=LOG_LENGTH):
    cmd = ["adb"]
    if serial:
        cmd += ["-s", serial]
    dd = (
        f"dd if={partition} bs={BLOCK} "
        f"skip={offset // BLOCK} count={length // BLOCK} 2>/dev/null"
    )
    if su:
        cmd += ["exec-out", "su", "-c", dd]
    else:
        cmd += ["exec-out"] + dd.split()
    return subprocess.run(cmd, check=True, stdout=subprocess.PIPE).stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--adb", action="store_true", help="pull the window over adb")
    src.add_argument("--input", help="read a raw window dump")
    ap.add_argument("--serial", help="adb serial (when several devices are attached)")
    ap.add_argument("--su", action="store_true", help="run dd through su (rooted Android)")
    ap.add_argument("--partition", default=LOG_PARTITION, help=f"block device (default {LOG_PARTITION})")
    ap.add_argument("--offset", type=lambda s: int(s, 0), default=LOG_OFFSET,
                    help=f"log offset (default {LOG_OFFSET:#x})")
    ap.add_argument("--length", type=lambda s: int(s, 0), default=LOG_LENGTH,
                    help=f"log window (default {LOG_LENGTH:#x})")
    ap.add_argument("--output", help="save the raw window here")
    ap.add_argument("--raw", action="store_true", help="write the raw window to stdout, not text")
    ap.add_argument("--quiet", action="store_true", help="do not print the summary line")
    args = ap.parse_args()

    if args.adb:
        data = read_adb(args.serial, args.su, args.partition, args.offset, args.length)
    else:
        data = Path(args.input).read_bytes()

    if len(data) != args.length:
        print(f"z3s-log: warning: read {len(data)} bytes, expected {args.length:#x}", file=sys.stderr)
    if args.output:
        Path(args.output).write_bytes(data)
        if not args.quiet:
            print(f"z3s-log: saved {len(data)} bytes to {args.output}", file=sys.stderr)

    if args.raw:
        sys.stdout.buffer.write(data)
        return

    text = decode_log(data)
    sys.stdout.write(text)
    if text and not text.endswith("\n"):
        sys.stdout.write("\n")
    if not args.quiet:
        print(f"z3s-log: {summarize(text)}", file=sys.stderr)


if __name__ == "__main__":
    main()
