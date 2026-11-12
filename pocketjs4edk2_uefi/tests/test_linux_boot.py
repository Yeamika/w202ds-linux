"""Validate actual Linux payload bounds and reject malformed raw image layouts."""
import ctypes as c
import os, struct, subprocess, tempfile, unittest
from pathlib import Path
import runpy

ROOT=Path(__file__).resolve().parents[1]
EDK2=runpy.run_path(str(ROOT/'tools/project_paths.py'))['edk2_directory']()
class Image(c.Structure):
    _fields_=[(n,c.c_uint64) for n in ('kernel','offset','ramdisk','entry','memory')]

class LinuxBootTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(dir=os.environ['W202_TEST_OUTPUT'])
        path=Path(cls.temp.name)
        wrapper=path/'linux.c'
        wrapper.write_text('#include <Uefi.h>\n#pragma GCC visibility pop\n#include "'+str(ROOT/'efi/LinuxBoot/LinuxImage.c')+'"\n')
        subprocess.run([os.environ.get('CC','cc'),'-shared','-fPIC','-fshort-wchar','-Wall','-Wextra','-Werror',
            '-I'+str(EDK2/'MdePkg/Include'),'-I'+str(EDK2/'MdePkg/Include/X64'),str(wrapper),'-o',str(path/'linux.so')],check=True)
        cls.lib=c.CDLL(str(path/'linux.so'));cls.parse=cls.lib.LinuxParseImage
        cls.parse.restype=c.c_uint64
        cls.parse.argtypes=[c.c_void_p,c.c_size_t,c.c_void_p,c.c_size_t,c.c_uint64,c.POINTER(Image)]
        cls.data=Path(os.environ['W202_LINUX_BOOT']).read_bytes()
        cls.header=cls.data[:4096];cls.kernel=cls.data[4096:4160]
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def run_parse(self,header=None,kernel=None,size=None):
        h=c.create_string_buffer(bytes(self.header if header is None else header))
        k=c.create_string_buffer(bytes(self.kernel if kernel is None else kernel));out=Image()
        status=self.parse(h,len(h)-1,k,len(k)-1,len(self.data) if size is None else size,c.byref(out))
        return status,out
    def test_actual_linux_image(self):
        status,out=self.run_parse();self.assertEqual(status,0)
        self.assertEqual(out.kernel,struct.unpack_from('<I',self.header,8)[0]);self.assertEqual(out.entry,0xd4000000)
        self.assertGreaterEqual(out.memory,out.kernel)
        self.assertLessEqual(out.offset+out.ramdisk,len(self.data))
    def test_truncation_and_payload_bounds(self):
        for h in [b'',self.header[:1583]]:self.assertNotEqual(self.run_parse(header=h)[0],0)
        self.assertNotEqual(self.run_parse(kernel=self.kernel[:63])[0],0)
        self.assertNotEqual(self.run_parse(size=len(self.data)-1)[0],0)
        self.assertNotEqual(self.run_parse(size=4096)[0],0)
    def test_bad_container(self):
        for off,value in [(0,0),(40,3),(20,0),(1580,1),(8,0xffffffff),(12,0),(12,0xffffffff)]:
            h=bytearray(self.header);struct.pack_into('<I',h,off,value)
            self.assertNotEqual(self.run_parse(header=h)[0],0)
    def test_kernel_placement(self):
        for off,value in [(8,0x200000),(16,0),(16,1),(16,0xffffffffffffffff),(24,2),(24,11),(56,0)]:
            k=bytearray(self.kernel);struct.pack_into('<Q',k,off,value)
            self.assertNotEqual(self.run_parse(kernel=k)[0],0)
        k=bytearray(self.kernel);struct.pack_into('<Q',k,8,0x80000)
        status,out=self.run_parse(kernel=k);self.assertEqual(status,0);self.assertEqual(out.entry,0xd4080000)

if __name__=='__main__':unittest.main()
