#!/usr/bin/env python3
"""Copy an IPA to Files > On My iPhone > Downloads for installation in TrollStore.

Uses the port's registered Jesvi Files container. Resumes partial SFTP uploads,
checks SHA-256, and reveals the .ipa only after verification. Never installs,
resprings, reboots, changes app permissions or replaces a different existing IPA.
"""
import argparse
import hashlib
from pathlib import Path
import re
import shlex
import subprocess


def remote(host, *args):
    return subprocess.check_output(["ssh", "-o", "ConnectTimeout=8", host,
                                    shlex.join(args)], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ipa", type=Path)
    parser.add_argument("--host", default="s20")
    parser.add_argument("--name", help="filename in Downloads; default is the source filename")
    args = parser.parse_args()
    source = args.ipa.resolve()
    name = args.name or source.name
    if not source.is_file() or source.suffix.lower() != ".ipa":
        raise ValueError("supply an existing .ipa file")
    if args.host.startswith("-") or not re.fullmatch(r"[\w.@-]+", args.host):
        raise ValueError("invalid SSH alias")
    if Path(name).name != name or not name.lower().endswith(".ipa"):
        raise ValueError("--name must be an IPA filename, without directories")
    if any(c in str(source) + name for c in "\r\n\0"):
        raise ValueError("control characters in filename")
    info = remote(args.host, "/var/jb/usr/bin/uicache", "-i", "org.hakios.LocalFilesHost")
    container = next((row.split(": ", 1)[1] for row in info.splitlines()
                      if row.startswith("Container Path: ")), "")
    if not re.fullmatch(r"/(?:private/)?var/mobile/Containers/Data/Application/[A-Fa-f0-9-]+", container):
        raise ValueError("registered Jesvi Files container not found")
    directory = container + "/Documents/Downloads"
    destination = directory + "/" + name
    partial = destination + ".partial"
    digest = hashlib.sha256()
    with source.open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(data)
    expected = digest.hexdigest()
    remote(args.host, "mkdir", "-p", directory)
    exists = subprocess.run(["ssh", args.host, shlex.join(["test", "-e", destination])]).returncode == 0
    if exists:
        actual = remote(args.host, "/var/jb/usr/bin/sha256sum", destination).split()[0]
        if actual == expected:
            print(f"Already verified: Files > On My iPhone > Downloads > {name}")
            return
        raise ValueError("a different IPA has that filename; choose another --name")
    if subprocess.run(["ssh", args.host, shlex.join(["test", "-L", partial])]).returncode == 0:
        raise ValueError("partial upload is a symlink; refusing")
    remote(args.host, "touch", partial)  # reput requires an existing destination
    def quote(value):
        return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"') + '"'
    print(f"Uploading {source.stat().st_size // 1024**2} MiB; rerun to resume if interrupted.", flush=True)
    subprocess.run(["sftp", "-b", "-", args.host], input=f"reput {quote(source)} {quote(partial)}\n",
                   text=True, check=True)
    actual = remote(args.host, "/var/jb/usr/bin/sha256sum", partial).split()[0]
    if actual != expected:
        raise ValueError("SHA-256 mismatch; upload retained as .partial for inspection")
    remote(args.host, "chown", "mobile:mobile", partial)
    remote(args.host, "chmod", "644", partial)
    remote(args.host, "mv", "-n", partial, destination)
    print(f"Verified: Files > On My iPhone > Downloads > {name}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error))
