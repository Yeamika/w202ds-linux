"""Boot v4 and ARM64 bounds using real captured inputs and malformed variants."""
import ctypes as c
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import runpy

ROOT=Path(__file__).resolve().parents[1]
EDK2=runpy.run_path(str(ROOT/'tools/project_paths.py'))['edk2_directory']()
class Boot(c.Structure):
    _fields_=[(n,c.c_uint64) for n in ('kernel_offset','kernel_bytes','ramdisk_offset','ramdisk_bytes')]
class Vendor(c.Structure):
    _fields_=[(n,c.c_uint64) for n in ('ramdisk_offset','ramdisk_bytes','table_offset')]
class Kernel(c.Structure):
    _fields_=[(n,c.c_uint64) for n in ('entry','memory_bytes')]

class AndroidBootTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(dir=os.environ.get('W202_TEST_OUTPUT'))
        so=Path(cls.temp.name)/'boot.so'
        wrapper=Path(cls.temp.name)/'boot.c'
        wrapper.write_text('#include <Uefi.h>\n#pragma GCC visibility pop\n#include "'+
            str(ROOT/'efi/Android/AndroidBoot/BootImage.c')+'"\n')
        subprocess.run([os.environ.get('CC','cc'),'-shared','-fPIC','-fshort-wchar','-Wall','-Wextra','-Werror',
            '-I'+str(EDK2/'MdePkg/Include'),'-I'+str(EDK2/'MdePkg/Include/X64'),
            str(wrapper),'-o',str(so)],check=True)
        cls.lib=c.CDLL(str(so))
        for name,kind in [('W202ParseBoot',Boot),('W202ParseVendor',Vendor),('W202ParseKernel',Kernel)]:
            fn=getattr(cls.lib,name);fn.restype=c.c_size_t
            fn.argtypes=[c.c_void_p,c.c_size_t,c.c_uint64,c.POINTER(kind)]
        boot=Path(os.environ['W202_TEST_BOOT'])
        vendor=Path(os.environ['W202_TEST_VENDOR_BOOT'])
        with boot.open('rb') as f:cls.boot=f.read(4096);cls.kernel=f.read(64)
        with vendor.open('rb') as f:cls.vendor=f.read(4096)
        cls.boot_size=boot.stat().st_size;cls.vendor_size=vendor.stat().st_size
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def parse(self,method,data,size,kind):
        out=kind();buf=c.create_string_buffer(bytes(data))
        return getattr(self.lib,method)(buf,len(data),size,c.byref(out)),out
    def test_actual_boot_vendor_and_relocated_kernel(self):
        status,b=self.parse('W202ParseBoot',self.boot,self.boot_size,Boot)
        self.assertEqual(status,0);self.assertEqual(b.kernel_bytes,39535104)
        status,v=self.parse('W202ParseVendor',self.vendor,self.vendor_size,Vendor)
        self.assertEqual(status,0);self.assertEqual(v.ramdisk_bytes+b.ramdisk_bytes+20,0x1976589)
        status,k=self.parse('W202ParseKernel',self.kernel,b.kernel_bytes,Kernel)
        self.assertEqual(status,0);self.assertEqual(k.entry,0xd4080000)
        self.assertLessEqual(k.entry+k.memory_bytes,0xd8000000)
    def test_boot_rejects_truncation_version_signature_and_overflow(self):
        variants=[self.boot[:1583]]
        for off,value in [(40,3),(20,1580),(8,0xffffffff),(12,0xffffffff),(1580,4096)]:
            h=bytearray(self.boot);struct.pack_into('<I',h,off,value);variants.append(h)
        for h in variants:self.assertNotEqual(self.parse('W202ParseBoot',h,self.boot_size,Boot)[0],0)
        self.assertNotEqual(self.parse('W202ParseBoot',self.boot,4096,Boot)[0],0)
    def test_vendor_rejects_unsupported_fragments_bootconfig_and_boundaries(self):
        for off,value in [(8,3),(12,2048),(24,0xffffffff),(2100,0xffffffff),(2116,2),(2120,4),(2124,1)]:
            h=bytearray(self.vendor);struct.pack_into('<I',h,off,value)
            self.assertNotEqual(self.parse('W202ParseVendor',h,self.vendor_size,Vendor)[0],0)
        self.assertNotEqual(self.parse('W202ParseVendor',self.vendor,4096,Vendor)[0],0)
    def test_kernel_rejects_nonrelocatable_compressed_and_out_of_range_images(self):
        for off,value in [(8,0x200000),(16,0),(16,1),(16,0xffffffffffffffff),(24,2),(24,11)]:
            h=bytearray(self.kernel);struct.pack_into('<Q',h,off,value)
            self.assertNotEqual(self.parse('W202ParseKernel',h,39535104,Kernel)[0],0)
        self.assertNotEqual(self.parse('W202ParseKernel',b'\x1f\x8b'+bytes(62),39535104,Kernel)[0],0)

if __name__=='__main__':unittest.main()
