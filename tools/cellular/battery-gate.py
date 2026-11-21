#!/usr/bin/env python3
"""Read only battery metadata; --require-ready gates repeated modem reboots."""
import argparse
import json
import re
import subprocess

KEYS = ('CurrentCapacity', 'MaxCapacity', 'Voltage', 'Amperage', 'IsCharging', 'ExternalConnected')


def decode(text):
    values = {}
    for key in KEYS:
        matches = re.findall(r'"' + key + r'"\s*=\s*(Yes|No|-?\d+)\s*$', text, re.M)
        if len(matches) != 1:
            raise ValueError('missing or ambiguous battery property: ' + key)
        value = matches[0]
        values[key] = (value == 'Yes') if value in ('Yes', 'No') else int(value)
    capacity, maximum = values['CurrentCapacity'], values['MaxCapacity']
    if type(capacity) is not int or type(maximum) is not int or not 0 <= capacity <= maximum or maximum <= 0:
        raise ValueError('invalid battery capacity')
    percent = capacity * 100 / maximum
    power_ready = ((values['Amperage'] > 0 and values['IsCharging'] is True) or
                   (percent >= 90 and values['Amperage'] >= 0))
    ready = (percent >= 10 and values['Voltage'] >= 3700 and power_ready
             and values['ExternalConnected'] is True)
    return dict(percent=percent, millivolts=values['Voltage'], milliamps=values['Amperage'],
                charging=values['IsCharging'], external_power=values['ExternalConnected'], ready=ready,
                requirement='at least10%,3700mV,external power; charging or near-full with nonnegative current')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--require-ready', action='store_true')
    args = parser.parse_args()
    result = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', 's20',
                             'ioreg -r -c IOPMPowerSource -l'], capture_output=True, text=True,
                            timeout=15, check=True)
    status = decode(result.stdout)
    print(json.dumps(status, indent=2))
    return 3 if args.require_ready and not status['ready'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
