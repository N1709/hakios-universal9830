#!/usr/bin/env python3
"""Read-only native usbmuxd/lockdown queries. Never pair or read credentials."""
import argparse
import plistlib
import socket
import struct


def exact(sock, count):
    data = bytearray()
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise RuntimeError("Native service closed the connection")
        data.extend(chunk)
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("device", type=int)
    args = parser.parse_args()
    request = plistlib.dumps({"MessageType": "Connect", "DeviceID": args.device, "PortNumber": socket.htons(62078), "ProgName": "jesvi-usb-probe", "ClientVersionString": "Jesvi transport probe", "kLibUSBMuxVersion": 3})
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(8)
        sock.connect("/var/run/usbmuxd")
        sock.sendall(struct.pack("<IIII", len(request) + 16, 1, 8, 1) + request)
        length, version, message, tag = struct.unpack("<IIII", exact(sock, 16))
        if not 16 <= length <= 65536 or (version, message, tag) != (1, 8, 1):
            raise RuntimeError("Unexpected native mux reply")
        reply = plistlib.loads(exact(sock, length - 16))
        if reply.get("Number") != 0:
            raise RuntimeError(f"Native mux refused connection: {reply.get('Number')}")
        for key in (None, "DeviceName", "ProductVersion"):
            query = {"Label": "Jesvi transport probe", "Request": "GetValue" if key else "QueryType"}
            if key:
                query["Key"] = key
            payload = plistlib.dumps(query)
            sock.sendall(struct.pack(">I", len(payload)) + payload)
            size = struct.unpack(">I", exact(sock, 4))[0]
            if not 0 < size <= 65536:
                raise RuntimeError("Native lockdown reply exceeds probe limit")
            reply = plistlib.loads(exact(sock, size))
            print(f"{key or 'QueryType'}: {reply.get('Value', reply.get('Type', reply.get('Error', 'Missing value')))}")


if __name__ == "__main__":
    main()
