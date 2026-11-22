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


def looks_textual(data):
    sample = data[:65536]
    if not sample:
        return False
    printable = sum(1 for b in sample if 9 <= b <= 13 or 32 <= b <= 126)
    return printable / len(sample) > 0.90


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
        if not any(m in text for m in ("log: ", "ufs: ", "xnuloader", "BootShim", "HakiOS", "XNU")):
            return "no HakiOS log: the window contains other data (recovery holds another image)"
        return "loader log only: the kernel did not return a log"
    if LOADER_END.decode() not in text:
        return "truncated: the kernel log has no end marker"
    return "loader log + kernel log"


def adb_is_root(serial=None):
    cmd = ["adb"]
    if serial:
        cmd += ["-s", serial]
    cmd += ["shell", "id", "-u"]
    out = subprocess.run(cmd, check=True, stdout=subprocess.PIPE).stdout.strip()
    return out == b"0"


def read_adb(serial=None, su=False, partition=LOG_PARTITION,
             offset=LOG_OFFSET, length=LOG_LENGTH):
    cmd = ["adb"]
    if serial:
        cmd += ["-s", serial]
    dd = (
        f"dd if={partition} bs={BLOCK} "
        f"skip={offset // BLOCK} count={length // BLOCK} 2>/dev/null"
    )
    # LineageOS userdebug has no `su` binary; `adb root` makes adbd uid 0.
    # Use the direct path whenever adbd is already root, even if --su was given.
    # Run through `sh -c` so `2>/dev/null` is a redirection on the phone, not
    # an argument to dd (adb exec-out does not use a shell by itself).
    if su and not adb_is_root(serial):
        cmd += ["exec-out", "su", "-c", dd]
    else:
        cmd += ["exec-out", "sh", "-c", dd]
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
    ap.add_argument("--allow-short", action="store_true",
                    help="accept a read shorter than --length (diagnostics only)")
    args = ap.parse_args()

    if args.adb:
        data = read_adb(args.serial, args.su, args.partition, args.offset, args.length)
    else:
        data = Path(args.input).read_bytes()

    if len(data) != args.length:
        print(f"z3s-log: warning: read {len(data)} bytes, expected {args.length:#x}", file=sys.stderr)
        if args.adb and not args.allow_short:
            if args.output:
                Path(args.output).write_bytes(data)
            print("z3s-log: error: short read; check `adb root`/`adb shell id -u` "
                  "or pass --allow-short to inspect the partial data", file=sys.stderr)
            raise SystemExit(2)
    if args.output:
        Path(args.output).write_bytes(data)
        if not args.quiet:
            print(f"z3s-log: saved {len(data)} bytes to {args.output}", file=sys.stderr)

    if args.raw:
        sys.stdout.buffer.write(data)
        return

    if not looks_textual(data):
        print("z3s-log: no HakiOS log: the window is not text "
              "(recovery holds another image, or the loader did not run)", file=sys.stderr)
        return

    text = decode_log(data)
    sys.stdout.write(text)
    if text and not text.endswith("\n"):
        sys.stdout.write("\n")
    if not args.quiet:
        print(f"z3s-log: {summarize(text)}", file=sys.stderr)


if __name__ == "__main__":
    main()
