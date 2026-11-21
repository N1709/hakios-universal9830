import plistlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import filesystem_checks as checks

class VirtualImageSafetyTests(unittest.TestCase):
    def scenario(self, *, existing=False, fsck_error=False, detach_error=False):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);recovery=root/'recovery';system=root/'system';kit=root/'kit'
            recovery.write_bytes(bytes(4096));system.write_bytes(bytes(4096))
            kit.write_bytes(b'HFSKIT01'+struct.pack('<IIQII',4096,1,8192,0,0)+struct.pack('<QII',0,1,1)+bytes(4096))
            calls=[]
            def execute(args, **kwargs):
                calls.append(args)
                if 'attach' in args:
                    data=plistlib.dumps({'system-entities':[{'dev-entry':'/dev/disk0' if existing else '/dev/disk99999'}]})
                    return type('R',(),{'returncode':0,'stdout':data,'stderr':b''})()
                failure=(args[0]=='/sbin/fsck_hfs' and fsck_error) or ('detach' in args and detach_error)
                return type('R',(),{'returncode':8 if failure else 0,'stdout':'test result\n','stderr':''})()
            device={'partitions':{'userdata':8192,'recovery':4160},'layout':{'root_offset':0}}
            with patch.object(checks,'ROOT',root),patch.object(checks,'hfs_kit'),patch.object(checks,'apfs',return_value=4096), \
                 patch.object(checks.Path,'is_file',return_value=True),patch.object(checks.Path,'glob',return_value=[Path('/dev/disk0')]), \
                 patch.object(checks.subprocess,'run',side_effect=execute):
                if existing or fsck_error or detach_error:
                    with self.assertRaises(ValueError):checks.verify(recovery,system,kit,device)
                else:checks.verify(recovery,system,kit,device)
            detaches=[a for a in calls if 'detach' in a]
            self.assertFalse(any('/dev/disk0' in a for a in detaches))
            remaining=list((root/'out/filesystem-checks').iterdir())
            if existing or detach_error:self.assertTrue(remaining)
            else:self.assertFalse(remaining)
            return detaches
    def test_failed_checker_detaches_only_its_own_virtual_image(self):
        self.assertEqual(self.scenario(fsck_error=True),[['/usr/bin/hdiutil','detach','/dev/disk99999']])
    def test_existing_device_is_never_detached(self):
        self.assertEqual(self.scenario(existing=True),[])
    def test_detach_failure_preserves_its_sparse_source_for_recovery(self):
        self.assertEqual(len(self.scenario(detach_error=True)),1)
