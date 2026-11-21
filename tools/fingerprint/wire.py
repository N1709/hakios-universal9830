"""FINGE research packets. No unlock, credential enrollment or HAT signing API."""
import struct

SIZE = 0x2EE00C
RESPONSE = 0x177008
ERROR = 0x2EE008
ENVELOPE = struct.Struct('<4sIIII')
LEGACY_STORE_PATH = 'fingerprint'
COMMANDS = (1, 2, 3, 4, 5, 6, 7, 9, 10, 12, 13)


def u32(packet, offset):
    return struct.unpack_from('<I', packet, offset)[0]


def put32(packet, offset, value):
    struct.pack_into('<I', packet, offset, value)


def request(command, operation=1, index=0):
    if command not in COMMANDS:
        raise ValueError('Unsupported research command')
    packet = bytearray(SIZE)
    for offset, value in ((0, command), (4, command), (8, index), (12, operation)):
        put32(packet, offset, value)
    return packet


def control(operation, data=b'', selector=b'', output_word=0):
    if operation in (0x16, 0x31, 0x5B, 0x5C, 0x6C):
        raise ValueError('Samsung client handles this operation locally; no TA request')
    if len(data) > 0xFA000 or len(selector) > 0x100 or not 0 <= output_word <= 0xFA000:
        raise ValueError('Control buffer exceeds wire capacity')
    packet = request(12, operation)
    packet[0x10:0x10 + len(selector)] = selector
    packet[0x110:0x110 + len(data)] = data
    put32(packet, 0xFA110, len(data))
    put32(packet, 0x271014, output_word)
    return packet


def storepath(path):
    value = path.encode('utf-8')
    if not value or len(value) >= 100 or b'\0' in value:
        raise ValueError('Storage path must fit a zero-terminated 100-byte field')
    return control(0x30, value.ljust(100, b'\0'))


def prepare(metadata=b'', operation=None):
    if len(metadata) > 0xA000:
        raise ValueError('PREPARE metadata exceeds wire capacity')
    packet = request(1, operation if operation is not None else (1 if metadata else 2))
    packet[0x10:0x10 + len(metadata)] = metadata
    put32(packet, 0xA010, len(metadata))
    return packet


def identify_init(template, metadata=b'', path=LEGACY_STORE_PATH):
    if not 0 < len(template) <= 0x64000 or len(metadata) > 0x400:
        raise ValueError('Identify input exceeds wire capacity or template is empty')
    value = path.encode('utf-8')
    if len(value) >= 100 or b'\0' in value:
        raise ValueError('Invalid storage path')
    packet = request(5)
    packet[0x10] = ord('0')
    packet[0x110:0x110 + len(template)] = template
    put32(packet, 0x64110, len(template))
    packet[0x64114:0x64114 + len(metadata)] = metadata
    put32(packet, 0x64514, len(metadata))
    packet[0x6455D:0x6455D + len(value)] = value
    return packet


def validate_request(packet):
    if len(packet) != SIZE or u32(packet, 0) != u32(packet, 4):
        raise ValueError('Invalid request length or command disagreement')
    cmd = u32(packet, 0)
    if cmd not in COMMANDS:
        raise ValueError('Unsupported research command')
    if cmd == 1 and u32(packet, 0xA010) > 0xA000:
        raise ValueError('Invalid PREPARE length')
    if cmd == 5 and (u32(packet, 0x64110) > 0x64000 or u32(packet, 0x64514) > 0x400):
        raise ValueError('Invalid identify length')
    if cmd == 12 and (u32(packet, 0xFA110) > 0xFA000 or u32(packet, 0x271014) > 0xFA000):
        raise ValueError('Invalid control length')
    if cmd == 13 and u32(packet, 0x451) > 0x400:
        raise ValueError('Invalid HAT metadata length')
    return cmd


def decode_reply(blob, expected_command):
    if len(blob) != SIZE + ENVELOPE.size:
        raise ValueError('Truncated or oversized research reply')
    magic, transport, origin, command, size = ENVELOPE.unpack_from(blob)
    if magic != b'FPR1' or size != SIZE or command != expected_command:
        raise ValueError('Reply envelope mismatch')
    packet = blob[ENVELOPE.size:]
    summary = dict(command=command, transport=transport, origin=origin)
    if transport:
        return summary, packet
    if u32(packet, 0x177004) != command:
        raise ValueError('TA response command mismatch')
    inner, outer = u32(packet, RESPONSE + 4), u32(packet, ERROR)
    if inner != outer:
        raise ValueError('TA error fields disagree')
    summary.update(operation=u32(packet, RESPONSE), error=outer)
    if command == 1:
        length = u32(packet, 0x181014)
        if length > 0xA000:
            raise ValueError('Invalid PREPARE output length')
        summary['sealed_metadata_bytes'] = length
    if command == 5:
        length = u32(packet, 0x1771A4)
        if length > 400:
            raise ValueError('Invalid identify ID-list length')
        summary['id_list_bytes'] = length
    if command == 6:
        summary.update(opaque_score=u32(packet, RESPONSE + 16), bio_id=u32(packet, RESPONSE + 20),
                       token_present=u32(packet, 0x2DB08D) == 1)
    return summary, packet
