#!/usr/bin/env python3
"""Summarize a private NMEA capture without printing location coordinates."""
import argparse
import json
import math
from pathlib import Path


def summarize(data):
    result = dict(sentences=0, checksum_errors=0, malformed=0, gga=0,
                  gga_valid=0, rmc=0, rmc_valid=0, satellites_used_max=0,
                  gsv=0, satellites_reported_max=0, signal_samples=0,
                  cn0_max_dbhz=0)
    for raw in data.splitlines():
        if not raw.startswith(b"$"):
            continue
        result["sentences"] += 1
        try:
            body, checksum = raw[1:].rsplit(b"*", 1)
            if len(checksum) != 2:
                raise ValueError("checksum length")
            actual = 0
            for byte in body:
                actual ^= byte
            if actual != int(checksum, 16):
                result["checksum_errors"] += 1
                continue
            fields = body.decode("ascii").split(",")
            if fields[0] in ("GPGGA", "GNGGA"):
                result["gga"] += 1
                quality, satellites = int(fields[6]), int(fields[7])
                if not 0 <= quality <= 8 or not 0 <= satellites <= 99:
                    raise ValueError("GGA bounds")
                result["gga_valid"] += quality != 0
                result["satellites_used_max"] = max(result["satellites_used_max"], satellites)
            elif fields[0] in ("GPRMC", "GNRMC"):
                result["rmc"] += 1
                if fields[2] not in ("A", "V"):
                    raise ValueError("RMC status")
                result["rmc_valid"] += fields[2] == "A"
            elif fields[0] in ("GPGSV", "GLGSV", "GAGSV", "GBGSV", "BDGSV", "GQGSV", "GNGSV"):
                total, index, reported = map(int, fields[1:4])
                if not 1 <= index <= total <= 99 or not 0 <= reported <= 999:
                    raise ValueError("GSV bounds")
                # Up to four SV quartets; NMEA 4.x may append a signal ID.
                count = (len(fields) - 4) // 4
                if count > 4 or (len(fields) - 4) % 4 not in (0, 1):
                    raise ValueError("GSV fields")
                signals = []
                for at in range(4, 4 + count * 4, 4):
                    if fields[at + 3]:
                        cn0 = float(fields[at + 3])
                        if not math.isfinite(cn0) or not 0 <= cn0 <= 99:
                            raise ValueError("GSV CN0 bounds")
                        if cn0 > 0:
                            signals.append(cn0)
                result["gsv"] += 1
                # Reported SVs can be predicted; only positive CN0 is counted
                # as a signal observation. Neither establishes a position fix.
                result["satellites_reported_max"] = max(result["satellites_reported_max"], reported)
                result["signal_samples"] += len(signals)
                result["cn0_max_dbhz"] = max([result["cn0_max_dbhz"]] + signals)
        except (ValueError, IndexError, UnicodeError):
            result["malformed"] += 1
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    args = parser.parse_args()
    print(json.dumps(summarize(args.capture.read_bytes()), sort_keys=True))
