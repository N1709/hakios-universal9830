#!/usr/bin/env python3
"""Read-only list from the Mac's existing usbmuxd; no pairing or credentials."""
import json
import plistlib
import socket
import struct


def read_exact(sock, count):
    data = bytearray()
    while len(data) < count:
        part = sock.recv(count - len(data))
        if not part:
            raise RuntimeError("usbmuxd closed the reply")
        data.extend(part)
    return data


def main():
    payload = plistlib.dumps({"MessageType": "ListDevices", "ClientVersionString": "Jesvi transport probe", "ProgName": "jesvi-usb-probe", "kLibUSBMuxVersion": 3})
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(5)
        sock.connect("/var/run/usbmuxd")
        sock.sendall(struct.pack("<IIII", 16 + len(payload), 1, 8, 1) + payload)
        length, version, message, tag = struct.unpack("<IIII", read_exact(sock, 16))
        if not 16 <= length <= 1024 * 1024 or (version, message, tag) != (1, 8, 1):
            raise RuntimeError("Unexpected usbmuxd reply header")
        reply = plistlib.loads(read_exact(sock, length - 16))
        for item in reply.get("DeviceList", []):
            props = item.get("Properties", {})
            # Whitelist public discovery fields; do not dump pairing records.
            print(json.dumps({"DeviceID": item.get("DeviceID"), **{key: props[key] for key in ("ConnectionType", "ProductID", "SerialNumber") if key in props}}))
        if not reply.get("DeviceList"):
            print("No devices reported by native usbmuxd")


if __name__ == "__main__":
    main()
