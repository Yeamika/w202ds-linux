"""Execute the actual patched standalone handlers with pinned native I/O ABI."""
import json
import os
from pathlib import Path
import struct
import unittest
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn import arm64_const as reg

class NativeFlashTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build=Path(os.environ['LK_FLASH_BUILD'])
        cls.image=(build/'lk-debuger.img').read_bytes()
        cls.symbols=json.loads((build/'manifest.json').read_text())['symbols']

    def run_handler(self,download=False,name='uboot_b',size=32768,capacity=32768,
                    mismatch=False,write_error=False,read_error=False,short=False,sparse=False):
        u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        for a,n in [(0xb5000000,0x1000000),(0x86000000,0x10000),
                    (0x8a000000,0x1000),(0xb6001000,0x1000)]:u.mem_map(a,n)
        u.mem_write(0xb5000000,self.image[512:0x113a60])
        u.mem_write(0x8a000000,(f'{size:08x}' if download else name).encode()+b'\0')
        payload=bytes(range(256))*128
        if sparse:payload=b'\x3a\xff\x26\xed'+payload[4:]
        u.mem_write(0x86000000,payload)
        u.mem_write(0xb5122048,struct.pack('<QQQ',0x86000000,0x10000,99))
        result={'write':0,'read':0,'reply':[],'tx':[]}
        storage=bytearray(capacity)
        callee={getattr(reg,f'UC_ARM64_REG_X{i}'):0x70000+i for i in range(18,31)}
        callee[reg.UC_ARM64_REG_X30]=0xb6001000
        for r,v in callee.items():u.reg_write(r,v)
        u.reg_write(reg.UC_ARM64_REG_SP,0xb5e00000)
        for r,v in [(reg.UC_ARM64_REG_X0,0x8a000000),
                    (reg.UC_ARM64_REG_X1,0x86000000),(reg.UC_ARM64_REG_X2,size)]:u.reg_write(r,v)
        def text(a):return bytes(u.mem_read(a,128)).split(b'\0',1)[0].decode()
        def ret(value=0):
            u.reg_write(reg.UC_ARM64_REG_X0,value&0xffffffffffffffff)
            u.reg_write(reg.UC_ARM64_REG_PC,u.reg_read(reg.UC_ARM64_REG_X30))
        def native(uc,pc,n,data):
            args=[uc.reg_read(getattr(reg,f'UC_ARM64_REG_X{i}')) for i in range(5)]
            if pc==0xb50403b8:
                self.assertEqual(text(args[0]),name)
                u.mem_write(args[1],struct.pack('<Q',capacity));ret()
            elif pc==0xb503f5d0:
                self.assertFalse(download,'download must never write storage')
                self.assertEqual(text(args[0]),'uboot_b')
                self.assertEqual(args[2],0)
                count,off,p=args[1],args[3],args[4]
                self.assertLessEqual(off+count,capacity)
                result['write']+=1
                if not write_error:storage[off:off+count]=u.mem_read(p,count)
                ret(-1 if write_error else 0)
            elif pc==0xb50405ec:
                count,off,p=args[1],args[2],args[3]
                self.assertEqual(text(args[0]),'uboot_b')
                self.assertLessEqual(off+count,capacity)
                result['read']+=1
                body=bytearray(storage[off:off+count])
                if mismatch:body[0]^=1
                u.mem_write(p,bytes(body));ret(-1 if read_error else 0)
            elif pc==0xb500e11c:
                result['reply'].append((text(args[0]),text(args[1])));ret()
            elif pc==0xb5081864:
                result['tx'].append(bytes(u.mem_read(args[0],args[1])));ret(args[1])
            elif pc==0xb5081638:
                self.assertTrue(download)
                self.assertEqual(args[:2],[0x86000000,size]);ret(size-1 if short else size)
        u.hook_add(UC_HOOK_CODE,native)
        offset=0xe750 if download else 0x100d4
        u.emu_start(0xb5000000+offset-512,0xb6001000,count=1000000)
        self.assertEqual(u.reg_read(reg.UC_ARM64_REG_PC),0xb6001000)
        self.assertEqual(u.reg_read(reg.UC_ARM64_REG_SP),0xb5e00000)
        for r,v in callee.items():self.assertEqual(u.reg_read(r),v)
        result['size']=struct.unpack('<Q',bytes(u.mem_read(0xb5122058,8)))[0]
        result['storage']=bytes(storage)
        return result

    def test_full_image_exact_target_and_readback(self):
        s=self.run_handler()
        self.assertEqual(s['reply'][0][0],'OKAY')
        self.assertEqual((s['write'],s['read']),(64,64))
        self.assertEqual(s['storage'],bytes(range(256))*128)

    def test_invalid_images_never_write(self):
        for args in [dict(size=512),dict(size=0),dict(name='uboot'),dict(name='esp'),dict(sparse=True)]:
            with self.subTest(args=args):
                s=self.run_handler(**args);self.assertEqual(s['reply'][0][0],'FAIL')
                self.assertEqual(s['write'],0)

    def test_io_failure_does_not_report_success_or_write_more(self):
        for args in [dict(mismatch=True),dict(write_error=True),dict(read_error=True)]:
            with self.subTest(args=args):
                s=self.run_handler(**args);self.assertEqual(s['reply'][0][0],'FAIL')
                self.assertEqual(s['write'],1)

    def test_raw_download_has_standard_data_response_and_no_storage(self):
        s=self.run_handler(download=True)
        self.assertEqual(s['tx'],[b'DATA00008000'])
        self.assertEqual(s['reply'],[('OKAY','')])
        self.assertEqual(s['size'],32768)
        s=self.run_handler(download=True,short=True)
        self.assertEqual(s['reply'][0][0],'FAIL');self.assertEqual(s['size'],0)

if __name__=='__main__':unittest.main()
