#!/usr/bin/env python3
"""Place the local Files provider in its registration host in a private image tree.

Usage: stage-provider.py SYSROOT HOST_EXECUTABLE INPUT_MANIFEST OUTPUT_MANIFEST
Only the supplied image staging tree is changed; input extraction is untouched.
"""
import os
from pathlib import Path
import plistlib
import shutil
import sys

PROVIDER = ('System/Library/Frameworks/FileProvider.framework/PlugIns/'
            'LocalStorageFileProvider.appex')
HOST = 'Applications/HakiosLocalFiles.app'
DESTINATION = HOST + '/PlugIns/LocalStorageFileProvider.appex'
PICKERS = ('SaveToFiles', 'com.apple.DocumentManager.Service')
PICKER_ROOT = 'System/Library/PrivateFrameworks/DocumentManagerUICore.framework/PlugIns/'


def stage(root, executable, manifest, output):
    root, executable = Path(root), Path(executable)
    if Path(manifest).resolve() == Path(output).resolve():
        raise SystemExit('Input and output manifests must be different files')
    source, destination = root / PROVIDER, root / DESTINATION
    if not (source / 'LocalStorageFileProvider').is_file():
        raise SystemExit('Local Files provider executable missing: rerun system selection/extraction from your IPSW')
    if not executable.is_file():
        raise SystemExit('LocalFilesHost missing: run tools/storage/build.sh and supply its output to prebuilt/userland')
    for name in PICKERS:
        if not (root / PICKER_ROOT / (name + '.appex') / name).is_file():
            raise SystemExit('Document picker executable missing: ' + name + '; refresh the IPSW extraction')
    if destination.exists():
        raise SystemExit(f'Refusing to replace an existing staged provider: {destination}')
    # Move within sysroot, whose hardlinks were made by build.sh. Do not edit
    # Apple's hardlinked files, or the source extraction would change too.
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(source), str(destination))
    relocations = {PROVIDER: DESTINATION}
    for name in PICKERS:
        old = PICKER_ROOT + name + '.appex'
        new = HOST + '/PlugIns/' + name + '.appex'
        shutil.move(str(root / old), str(root / new))
        relocations[old] = new
    host = root / HOST
    shutil.copyfile(executable, host / 'LocalFilesHost')
    os.chmod(host / 'LocalFilesHost', 0o755)
    with (host / 'Info.plist').open('wb') as stream:
        plistlib.dump({
            'CFBundleIdentifier': 'org.hakios.LocalFilesHost',
            'CFBundleExecutable': 'LocalFilesHost',
            'CFBundleName': 'Jesvi Files',
            'CFBundleDisplayName': 'Jesvi Files',
            'CFBundlePackageType': 'APPL',
            'CFBundleVersion': '1',
            'CFBundleShortVersionString': '1.0',
            'LSRequiresIPhoneOS': True,
            'MinimumOSVersion': '16.0',
            'UIDeviceFamily': [1, 2],
            'UIRequiredDeviceCapabilities': ['arm64'],
            'SBAppTags': ['hidden'],
            'UIFileSharingEnabled': True,
            # (not LSSupportsOpeningDocumentsInPlace: its Documents IS On My iPhone, by a link that
            # register_finder_host makes, and the key would list that storage inside itself)
        }, stream)
    # Preserve the extension's owners, links and empty localization directories
    # at the new path; otherwise mkapfs would recreate its old framework path.
    with open(manifest) as original, open(output, 'w') as updated:
        for line in original:
            fields = line.rstrip('\n').split('\t')
            if len(fields) >= 4:
                for old, new in relocations.items():
                    if fields[3] == old or fields[3].startswith(old + '/'):
                        fields[3] = new + fields[3][len(old):]
                        break
            updated.write('\t'.join(fields) + '\n')
    print(f'Local Files: staged provider in {HOST} (one copy)')


if __name__ == '__main__':
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    stage(*sys.argv[1:])
