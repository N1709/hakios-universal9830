# Structured FINGE research exchange

This is a manually armed debugging client on `feat/s20-fingerprint`. It is not
an iOS biometric provider or unlock API. The running exchange-v2 kernel supports it, installed with explicit approval.
An older audit-v1 kernel cannot use it; the CLI refuses that marker.

The previous session's cache hid the absence of a real disk-load test. After the
audit reboot the outer integrity check failed because the legacy storage path
was not restored. Supplying the exact original value ("fingerprint") fixed the
load; a later fresh enrollment also passed a forced disk reload. Matching still
fails. Preserve original/fresh files, factory calibration and passcode. See
FINGERPRINT-HANDOVER.md before further experiments.

## Files and synchronization

- On the phone: `/var/root/fp-research`, root-owned mode 0700.
- Request: `request.bin`, exactly `0x2ee00c` bytes, root-owned regular single-link
  file, mode 0600. The kernel rejects other types, symlinks, sizes and modes.
- Reply: `reply.bin`, same permissions. A 20-byte little-endian envelope contains
  `FPR1`, transport result, origin, command and packet size, then the full packet.
- Kernel command: `sysctl -w kern.redwood.tee='fp exchange'`, only after the
  version/idle check. It invokes one request, writes the reply and clears its
  shared buffer. It never logs template, calibration, HAT or metadata bytes.
- Kernel `fp` commands return EBUSY while another fingerprint command is active.
  The host takes a non-blocking lock in `~/.cache/jesvi-fingerprint` across all
  checkouts on this Mac. No other session should write the request/reply files.
- The host refuses audit-v1 or a busy kernel **before uploading**. Do not send
  an unknown command to audit-v1: its old generic parser does not reject words.

`wire.py` constructs complete, zeroed requests using the decompiled client ABI.
It has helpers for PREPARE, CONTROL, SEND_STOREPATH and Identify Init.
Samsung's client intercepts operations `0x16`, `0x31`, `0x5b`, `0x5c`, `0x6c`;
the CONTROL builder refuses to turn these client-local operations into TA calls.
A TA-side 0x16 handler also exists, but its existence does not make it part of
the stock client flow. CMD12 input length is at `0xfa110`, unlike PREPARE's
`0xa010`. CMD13 is reserved for a genuine hardware auth token; this tooling
neither generates nor signs one and validates the optional metadata size.

## Example: prepare a request without contacting the phone

From the repository root:

```sh
umask 077
mkdir -p out/fingerprint/private
chmod 700 out/fingerprint/private
python3 - <<'PY'
import os, sys
sys.path.insert(0, 'tools/fingerprint')
import wire
# The old ENROLL_INIT supplied "fingerprint" as its logical storage path.
# Restore that exact legacy value for existing files; do not rename it.
packet = wire.storepath(wire.LEGACY_STORE_PATH)
fd = os.open('out/fingerprint/private/path-request.bin',
             os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
with os.fdopen(fd, 'wb') as output:
    output.write(packet)
PY
```

Only during an agreed research session, with the phone lock held and the
exchange-v2 marker present, send one explicitly prepared request:

```sh
python3 tools/fingerprint/exchange.py \
  out/fingerprint/private/path-request.bin \
  out/fingerprint/private/path-reply.bin
```

That command changes the TA's storage-path context. It does not rewrite or
re-sign a template, verify a finger or unlock the phone. A path must be preserved
consistently over the entire enrollment/store/restore lifecycle. Never use a new
path to claim that an old integrity-failing template has been repaired.

The CLI prints only command/status, errors and bounded diagnostic fields; it
writes the full reply to a new mode-0600 local file in a mode-0700 directory.
Treat replies as sensitive because they may contain templates or HATs. Never
commit or paste them. It refuses to overwrite an existing local reply, and it
does not repeat commands on timeouts. A timeout means the request may still be
running; inspect status and retain the phone lock before doing anything else.

## Planned experiments

1. Validate the bridge's file permissions, malformed-request rejection and
   command serialization on-device before passing biometric payloads.
2. Investigate existing file integrity with controlled path/context inputs.
   A mismatch must stay a failure. Do not export the secure KDF/HAT key.
3. Restore/save the correct sealed PREPARE and secure-ID metadata according to
   the TA/client protocol; first establish which output each operation provides.
4. If a new enrollment is needed, obtain user readiness and preserve the old
   file. Compare newly saved bytes against a forced disk read within the same
   boot, then compare using the TA. Kernel-cache reuse is not a persistence test.
5. A real same-finger match and a different-finger rejection are required before
   tackling authenticated iOS integration. Never lower thresholds or accept a
   candidate/token-present flag as a verified unlock result.

Tests (synthetic; no device operations):

```sh
python3 tools/fingerprint/test_exchange.py
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I xnu/files/bsd tools/fingerprint/test_wire.c -o out/fingerprint/test-wire
out/fingerprint/test-wire
```
