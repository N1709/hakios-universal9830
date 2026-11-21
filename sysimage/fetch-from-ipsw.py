#!/usr/bin/env python3
"""Fetch one member of the iOS 16.5 iPhone X firmware archive without downloading the rest.

usage: fetch-from-ipsw.py MEMBER OUTPUT

The IPSW is a ZIP64 archive on Apple's CDN; its directory is read with HTTP range requests and
the member is streamed (and inflated, if it is compressed) straight to OUTPUT. A download that
breaks off is taken up again: a member stored without compression (the big disk images are)
continues where it stopped, also when OUTPUT is the partial file of an earlier run; a compressed
one starts over.
"""
import http.client
import os
import struct
import sys
import time
import urllib.request
import zlib

ATTEMPTS = 20

URL = ('https://updates.cdn-apple.com/2023SpringFCS/fullrestores/032-85130/6AB0D10A-8AF9-4F6C-8CF5-A1B315CABE88/'
       'iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw')
SIZE = 6165698198
# Another firmware of the same build, e.g. the iPhone 8's for Touch ID's libBKDM1 (tools/touchid/fetch-bkdm1.sh):
# IPSW_URL=... IPSW_SIZE=... fetch-from-ipsw.py MEMBER OUTPUT;  MEMBER "-" lists the archive's members.
URL = os.environ.get('IPSW_URL', URL)
SIZE = int(os.environ.get('IPSW_SIZE', SIZE))


def fetch(start, end):
    request = urllib.request.Request(URL, headers={'Range': f'bytes={start}-{end}'})
    response = urllib.request.urlopen(request, timeout=120)
    expected = f'bytes {start}-{end}/{SIZE}'
    if response.status != 206 or response.headers.get('Content-Range') != expected:
        response.close()
        raise ValueError('Apple CDN did not honor the requested byte range')
    return response


def directory():
    tail = fetch(SIZE - 65536, SIZE - 1).read()
    at = tail.rfind(b'PK\x06\x06')
    cd_size, cd_offset = struct.unpack('<QQ', tail[at + 40:at + 56])
    cd = fetch(cd_offset, cd_offset + cd_size - 1).read()
    p = 0
    while cd[p:p + 4] == b'PK\x01\x02':
        method, = struct.unpack('<H', cd[p + 10:p + 12])
        csize, usize, nl, el, cl = struct.unpack('<IIHHH', cd[p + 20:p + 34])
        offset, = struct.unpack('<I', cd[p + 42:p + 46])
        name = cd[p + 46:p + 46 + nl].decode()
        extra = cd[p + 46 + nl:p + 46 + nl + el]
        q = 0
        while q + 4 <= len(extra):
            tag, length = struct.unpack('<HH', extra[q:q + 4])
            if tag == 1:
                values = list(struct.unpack('<' + 'Q' * (length // 8), extra[q + 4:q + 4 + length // 8 * 8]))
                if usize == 0xFFFFFFFF:
                    usize = values.pop(0)
                if csize == 0xFFFFFFFF:
                    csize = values.pop(0)
                if offset == 0xFFFFFFFF:
                    offset = values.pop(0)
            q += 4 + length
        crc, = struct.unpack('<I', cd[p + 16:p + 20])
        yield name, method, csize, usize, offset, crc
        p += 46 + nl + el + cl


def download(data, method, csize, output):
    """One attempt at the member whose bytes start at DATA in the archive; returns the size of OUTPUT."""
    if method == 0:
        have = os.path.getsize(output) if os.path.exists(output) else 0
        if have > csize:
            have = 0
        if have < csize:
            with fetch(data + have, data + csize - 1) as stream, open(output, 'ab' if have else 'wb') as out:
                while chunk := stream.read(1 << 20):
                    out.write(chunk)
        return os.path.getsize(output)
    inflater = zlib.decompressobj(-15)
    written = 0
    with fetch(data, data + csize - 1) as stream, open(output, 'wb') as out:
        while chunk := stream.read(1 << 20):
            chunk = inflater.decompress(chunk)
            out.write(chunk)
            written += len(chunk)
        tail = inflater.flush()
        out.write(tail)
    return written + len(tail)


def main():
    member, output = sys.argv[1], sys.argv[2]
    if member == '-':
        for name, method, csize, usize, offset, expected_crc in directory():
            print(f'{usize:>12} {name}')
        return
    for name, method, csize, usize, offset, expected_crc in directory():
        if name != member:
            continue
        if method not in (0, 8):
            sys.exit(f'{member}: compression method {method} is not supported')
        header = fetch(offset, offset + 29).read()
        nl, el = struct.unpack('<HH', header[26:30])
        data = offset + 30 + nl + el
        written = -1
        for attempt in range(1, ATTEMPTS + 1):
            try:
                written = download(data, method, csize, output)
            except (OSError, http.client.HTTPException, zlib.error) as error:
                print(f'{member}: {error}', file=sys.stderr)
            if written == usize:
                break
            print(f'{member}: the download broke off, trying again ({attempt} of {ATTEMPTS})', file=sys.stderr)
            time.sleep(5)
        if written == usize:
            crc = 0
            with open(output, 'rb') as completed:
                for block in iter(lambda: completed.read(8 * 1024 * 1024), b''):
                    crc = zlib.crc32(block, crc)
            if crc & 0xffffffff != expected_crc:
                sys.exit(f'{member}: ZIP CRC mismatch; quarantine {output} before retrying')
        print(f'{member}: {written} bytes (expected {usize})')
        sys.exit(0 if written == usize else 1)
    sys.exit(f'{member}: not in the archive')


if __name__ == '__main__':
    main()
