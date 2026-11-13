"""Verify menu transfers are bounded and never publish partial RAM as complete."""
from pathlib import Path
import runpy
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

API=runpy.run_path(str(Path(__file__).with_name('read.py')))

class ChunkTest(unittest.TestCase):
    def run_reader(self, native=False, fail=False):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);module=root/'module.bin';out=root/'out'
            module.write_bytes(struct.pack('<IIIIQQ',0,32,0x314b4c50,2,0,0))
            requests=[];active=None
            def run(argv,**unused):
                nonlocal active
                if argv[-2:]==['getvar','version']:
                    return subprocess.CompletedProcess(argv,0,'','version: '+('1.0' if native else '0.4'))
                if argv[-2]=='stage':
                    b=Path(argv[-1]).read_bytes();active=struct.unpack_from('<QQ',b,16)
                    self.assertEqual(struct.unpack_from('<I',b,12)[0],1 if native else 2)
                    return subprocess.CompletedProcess(argv,0,'','')
                self.assertEqual(argv[-3:],['oem','jump-rmk',':probe'])
                address,size=active;requests.append(active)
                data=bytes((address+i)&255 for i in range(size))
                lines=[f'PJ4EDK2 BEGIN {address:08x} {size:08x}']
                lines += [f'PJ4EDK2 MEM {address+i:08x} {data[i:i+16].hex()}' for i in range(0,size,16)]
                lines += [f'PJ4EDK2 CRC {zlib.crc32(data):08x}']
                if fail and len(requests)==2:
                    raise subprocess.TimeoutExpired(argv,45,stderr=b'INFO partial second chunk')
                return subprocess.CompletedProcess(argv,0,'','\n'.join(lines))
            argv=['read.py','--serial','test','--module',str(module),'--address','0x80000000','--size','0x410','--output',str(out)]
            with patch.object(sys,'argv',argv),patch.object(subprocess,'run',run):
                if fail:
                    with self.assertRaisesRegex(RuntimeError,'partial output saved'):API['main']()
                    self.assertFalse((out/'memory.bin').exists())
                    self.assertIn('partial second chunk',(out/'fastboot.log').read_text())
                else:
                    API['main']()
                    self.assertEqual((out/'memory.bin').read_bytes(),bytes(i&255 for i in range(0x410)))
            return requests

    def test_menu_uses_bounded_chunks_and_reassembles_final_short_chunk(self):
        self.assertEqual(self.run_reader(),[(0x80000000,512),(0x80000200,512),(0x80000400,16)])

    def test_native_retains_single_transfer_and_adapts_header(self):
        self.assertEqual(self.run_reader(native=True),[(0x80000000,1040)])

    def test_timeout_keeps_partial_log_without_a_successful_dump(self):
        self.run_reader(fail=True)

if __name__=='__main__':unittest.main()
