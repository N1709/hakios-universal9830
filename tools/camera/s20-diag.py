#!/usr/bin/env python3
"""Collect a read-only Galaxy S20 camera bring-up snapshot.

    python3 tools/camera/s20-diag.py [--out PATH]

Requires the existing `ssh s20` alias. This does not change boot arguments,
reboot, respring, power rails, clocks, GPIO, or camera application state.
"""
from __future__ import annotations

import argparse
import datetime as dt
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def run(argv: list[str], timeout: int = 20) -> tuple[int, str]:
    try:
        p = subprocess.run(argv, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=timeout)
        return p.returncode, p.stdout.rstrip()
    except (OSError, subprocess.TimeoutExpired) as e:
        return 127, f"ERROR: {e}"


def section(name: str, result: tuple[int, str]) -> str:
    code, body = result
    return f"## {name} (exit {code})\n{body or '(no output)'}\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", help="report path (default: logs/camera/s20-TIMESTAMP.txt)")
    args = ap.parse_args()
    now = dt.datetime.now().astimezone()
    stamp = now.strftime("%Y%m%d-%H%M%S")
    out = pathlib.Path(args.out) if args.out else ROOT / "logs/camera" / f"s20-{stamp}.txt"

    parts = [f"# S20 camera diagnostic snapshot\nTime: {now.isoformat()}\nHost: {os.uname().nodename}\n"]
    parts += [section("Repository", run(["git", "status", "--short", "--branch"])),
              section("Current revision", run(["git", "log", "-1", "--format=%H %s"])),
              section("Kernel build artifacts", run(["sh", "-c",
                      "ls -lT out/mach.x1slte out/x1slte-recovery.img; shasum -a 256 out/mach.x1slte out/x1slte-recovery.img"]))]

    remote = r'''set +e
echo '--- kernel ---'
uname -a
echo '--- device-tree boot arguments and camera nodes ---'
ioreg -p IODeviceTree -l 2>&1 | grep -iE 'boot-args|camera|csis|hsi2c' | head -100
echo '--- camera nodes ---'
ls -ld /dev/redwood-camera /dev/hakios-camera /dev/video0 2>&1
echo '--- Camera.app bundle ---'
ls -ld /Applications/Camera.app /Applications/Camera.app/Camera /Applications/Camera.app/Info.plist 2>&1
echo '--- MobileGestalt camera capabilities ---'
/redwood/bin/mg_test front-facing-camera rear-facing-camera still-camera 2>&1
echo '--- SpringBoard icon reference ---'
grep -a -o 'com.apple.camera' /var/mobile/Library/SpringBoard/IconState.plist 2>/dev/null | head -5
echo '--- relevant processes ---'
ps -A | grep -E 'SpringBoard|backboardd|mediaserverd|Camera' | head -30
echo '--- recent media/camera unified log ---'
log show --last 10m --style compact --predicate '(process == "mediaserverd") OR (process == "SpringBoard")' 2>&1 | grep -iE 'camera|capture|sensor|media|error|fail' | tail -160
'''
    parts.append(section("Phone snapshot (read-only)", run(["ssh", "-o", "BatchMode=yes",
                  "-o", "ConnectTimeout=5", "s20", remote], timeout=45)))

    kernel_log_path = out.with_suffix(".kernel.log")
    rc, ip = run(["phone-ip"], timeout=5)
    if rc == 0 and ip:
        kr, kernel_log = run(["curl", "-fsS", "--connect-timeout", "3", "--max-time", "20",
                              f"http://{ip}:8080/log"], timeout=25)
        if kr == 0:
            kernel_log_path.parent.mkdir(parents=True, exist_ok=True)
            kernel_log_path.write_text(kernel_log + "\n")
            lower = ("camera", "cam_s20", "csis", "sensor", "hsi2c", "mg_test:",
                     "reset: last run", "boot-args =")
            relevant = "\n".join(line for line in kernel_log.splitlines()
                                   if any(key in line.lower() for key in lower))
            parts.append(section(f"Camera-related kernel log lines ({ip})",
                                 (0, f"Full log: {kernel_log_path}\n{relevant or '(no camera-related lines yet)'}")))
        else:
            parts.append(section(f"HackiOS kernel log ({ip})", (kr, kernel_log)))
    else:
        parts.append(section("HackiOS kernel log", (rc, f"phone-ip unavailable: {ip}")))

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(parts) + "\n")
    print(f"camera diagnostics saved: {out}")
    print("Read-only collection complete; no phone state was changed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
