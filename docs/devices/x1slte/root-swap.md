# Changing only root.apfs on the phone (no loader/kernel/signature change)

Use when the phone's recovery image was flashed from somewhere you cannot rebuild exactly (another worktree or
session) and you only need to change files of the small root (`/redwood`: `lib/`, `LaunchDaemons/`, `sbin/init`,
`etc/`). A, 2026-11-15 (three flashes that night, all clean).

Layout of the recovery image (69 009 408 bytes): loader/payload at 0 (the signed part, with the boot line and the
`rsize` of root.apfs), kernel at 32 MiB, **root.apfs at 52 MiB (0x3400000), outside the signed range**.

1. Hold the phone lock. Copy the phone's image and check it is the one on the phone:
   `ssh s20 'cat /dev/rufs_recovery' > phone-recovery.img`; `ssh s20 'shasum -a 256 /dev/rufs_recovery'`.
2. Read `rsize` from the payload table at 0x100870+8 (entries: 16-byte name, u32 size, 4 pad, value padded to 16):
   the new root.apfs **must be exactly this size** or the loader would need a new payload (a re-sign).
3. Start from the staged tree (`userland/root`), compare it with the phone (`ssh s20 'cd /redwood && /var/jb/usr/bin/find
   . -type f -exec /var/jb/usr/bin/sha256sum {} +'`), copy the phone's own version of every file that differs
   (2026-11-15: `sbin/init`, `etc/var.skel`, `etc/motd`), then add only your changes. Diff again: only the intended files.
4. `python3 tools/apfs/mkapfs.py -z --plain fw -o new-root.apfs redwood=DIR` (it makes a fixed-size container: so far
   always 13 467 648 bytes). Check files with `tools/apfs/apfs.py cat new-root.apfs /PATH -v redwood`.
5. Splice it at 0x3400000 into the phone's image; assert everything outside [0x3400000, +rsize) is byte-identical.
6. Put it at `out/x1slte-recovery.img` (keep a backup) and `CLAUDE_SESSION=NAME tools/x1slte-auto.sh --recovery`
   (Log access -> uflash -> restart; iOS back in ~32 s).

Traps met on the way:
- The libraries need the xnu-deps compiler on PATH: `export PATH=$PWD/xnu-deps/host/bin:$PATH` (xnu-clang). Without
  it the compile "succeeds" silently with no output file. `build-ims.sh` and telephonyd's build use plain Xcode clang.
- zsh: `"$var:r..."` is a filename modifier (`:r` strips an extension). Write `"${var}:refs/heads/x"`.
- `tools/apfs/check-image.sh` currently fails to build (apfsck); use `apfs.py` to read the image back instead.
- The phone has no `shasum`/`pgrep`/`awk`; use `/var/jb/usr/bin/sha256sum`, `/var/jb/usr/bin/find`.
