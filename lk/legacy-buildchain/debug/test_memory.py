"""Execute the real PLK1 reader and validate its Fastboot packet stream."""
import os
from pathlib import Path
import runpy
import struct
import unittest
import zlib

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1

API = runpy.run_path(str(Path(__file__).with_name('read.py')))
BASE, ARG, STACK, END, DDR = 0x86000000, 0x8a001000, 0xb5e00000, 0xb6001000, 0x82ffe000


class MemoryTest(unittest.TestCase):
    def run_reader(self, address=DDR, size=0x2000, operation=':probe'):
        module = bytearray((Path(os.environ['LK_MEMORY_BUILD'])/'ramboot-memory.bin').read_bytes())
        struct.pack_into('<QQ', module, 16, address, size)
        u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        for start, length in ((BASE,0x200000),(ARG,0x1000),(STACK-0x10000,0x10000),
                              (END,0x1000),(0xb500e000,0x1000),(DDR,0x2000)):
            u.mem_map(start,length)
        data = bytes(i % 251 for i in range(0x2000))
        if 0xc8000000 <= address < 0xcc800000:
            u.mem_map(address & ~0xfff, 0x3000)
            u.mem_write(address,data)
        u.mem_write(BASE,bytes(module))
        u.mem_write(DDR,data)
        u.mem_write(ARG,operation.encode()+b'\0')
        u.reg_write(UC_ARM64_REG_SP,STACK)
        u.reg_write(UC_ARM64_REG_LR,END)
        u.reg_write(UC_ARM64_REG_X0,ARG)
        packets=[]
        def status(uc, pc, count, state):
            if pc != 0xb500e11c:
                return
            prefix = bytes(uc.mem_read(uc.reg_read(UC_ARM64_REG_X0),5)).split(b'\0')[0]
            text = bytes(uc.mem_read(uc.reg_read(UC_ARM64_REG_X1),60)).split(b'\0')[0]
            self.assertEqual(prefix,b'INFO')
            self.assertLessEqual(len(text)+4,63)
            packets.append(text.decode())
            uc.reg_write(UC_ARM64_REG_PC,uc.reg_read(UC_ARM64_REG_LR))
        u.hook_add(UC_HOOK_CODE,status)
        u.emu_start(BASE,END,count=2000000)
        self.assertEqual(u.reg_read(UC_ARM64_REG_PC),END)
        self.assertEqual(u.reg_read(UC_ARM64_REG_SP),STACK)
        self.assertEqual(bytes(u.mem_read(DDR,len(data))),data)
        return '\n'.join('(bootloader) '+x for x in packets),data,u.reg_read(UC_ARM64_REG_X0)

    def test_reader_returns_all_bytes_with_a_valid_checksum(self):
        text,data,result=self.run_reader()
        self.assertEqual(result,0)
        self.assertEqual(API['decode_packets'](text,DDR,len(data)),data)

    def test_invalid_ranges_and_operations_return_without_reading(self):
        for args in (dict(address=DDR+1),dict(address=0xc0000000),dict(size=0),
                     dict(address=0xcc800000),dict(address=0xcc7ffff0,size=32),
                     dict(size=0x10010),dict(address=0xbffffff0,size=32),dict(operation=':init')):
            with self.subTest(**args):
                text,_,result=self.run_reader(**args)
                self.assertFalse(text)
                self.assertNotEqual(result,0)

    def test_extra_uefi_bank_is_readable(self):
        text,data,result=self.run_reader(address=0xcb540000)
        self.assertEqual(result,0)
        self.assertEqual(API['decode_packets'](text,0xcb540000,len(data)),data)

    def test_missing_duplicate_and_corrupt_usb_packets_are_rejected(self):
        text,data,_=self.run_reader()
        lines=text.splitlines()
        for bad in ('\n'.join(lines[:2]+lines[3:]),text+'\n'+lines[1],
                    text.replace('PJ4EDK2 CRC ', 'PJ4EDK2 CRC f',1)):
            with self.subTest():
                with self.assertRaises(ValueError):API['decode_packets'](bad,DDR,len(data))

    def test_trace_ring_is_decoded_in_order(self):
        data=bytearray(8192)
        struct.pack_into('<8Q',data,4096,0x57323032484e4431,0x12,8,0,0,0,0x82806e78,4034)
        data[4160:]=b'bc'+b'a'*4030
        trace,log=API['decode_trace'](data,DDR)
        self.assertTrue(trace['valid'])
        self.assertEqual(trace['stage_name'],'before-native-cleanup')
        self.assertEqual(log,b'a'*4030+b'bc')


if __name__=='__main__':unittest.main()
