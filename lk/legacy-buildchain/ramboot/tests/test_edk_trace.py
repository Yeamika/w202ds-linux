#!/usr/bin/env python3
"""Test the compiled SEC RAM-log writer with emulated cache maintenance."""
import os
from pathlib import Path
import struct
import subprocess
import unittest

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_LR, UC_ARM64_REG_PC
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1

CODE = 0x100000
END = 0x200000
ARG = 0x300000
TRACE = 0x82FFF000
CAPACITY = 4032
MAGIC = 0x57323032484E4431


class EdkTraceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = os.environ["RAMBOOT_SEC_ELF"]
        data = Path(path).read_bytes()
        if data[:6] != b"\x7fELF\x02\x01":
            raise ValueError("expected a little-endian ELF64 SEC binary")
        shoff = struct.unpack_from("<Q", data, 40)[0]
        size, count, names_index = struct.unpack_from("<HHH", data, 58)
        sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * size)
                    for i in range(count)]
        names = sections[names_index]
        strings = data[names[4]:names[4] + names[5]]
        text = next(section for section in sections
                    if strings[section[0]:].split(b"\0", 1)[0] == b".text")
        cls.text_address = text[3]
        cls.text = data[text[4]:text[4] + text[5]]
        symbols = subprocess.check_output([os.environ["AARCH64_NM"], "-n", path], text=True)
        cls.writer = next(int(line.split()[0], 16) for line in symbols.splitlines()
                          if line.split()[-1].startswith("SerialPortWrite"))

    def write_log(self, payload, initial=b"", magic=MAGIC):
        u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.mem_map(CODE, 0x20000)
        u.mem_map(END, 0x1000)
        u.mem_map(ARG, 0x10000)
        u.mem_map(TRACE, 0x1000)
        u.mem_write(CODE + self.text_address, self.text)
        u.mem_write(ARG, payload)
        ring = bytearray(CAPACITY)
        for index, byte in enumerate(initial):
            ring[index % CAPACITY] = byte
        u.mem_write(TRACE, struct.pack("<QQQQQQQQ", magic, 0, 0, 0, 0, 0, 0,
                                      len(initial)) + ring)
        u.reg_write(UC_ARM64_REG_X0, ARG)
        u.reg_write(UC_ARM64_REG_X1, len(payload))
        u.reg_write(UC_ARM64_REG_LR, END)

        def instruction(uc, address, _size, _data):
            insn = struct.unpack("<I", bytes(uc.mem_read(address, 4)))[0]
            if insn & ~31 == 0xD53B0020:
                uc.reg_write(UC_ARM64_REG_X0 + (insn & 31), 4 << 16)
                uc.reg_write(UC_ARM64_REG_PC, address + 4)
            elif insn & ~31 == 0xD50B7A20:
                uc.reg_write(UC_ARM64_REG_PC, address + 4)

        u.hook_add(UC_HOOK_CODE, instruction, begin=CODE + self.writer,
                   end=CODE + self.writer + 0x300)
        u.emu_start(CODE + self.writer, END, count=1000000)
        self.assertEqual(u.reg_read(UC_ARM64_REG_PC), END)
        page = bytes(u.mem_read(TRACE, 4096))
        total = struct.unpack_from("<Q", page, 56)[0]
        count = min(total, CAPACITY)
        start = total % CAPACITY if total >= CAPACITY else 0
        ring = page[64:]
        return total, bytes(ring[(start + i) % CAPACITY] for i in range(count))

    def test_append_and_wrap(self):
        initial = b"a" * 4000
        payload = b"b" * 100
        total, output = self.write_log(payload, initial)
        self.assertEqual(total, 4100)
        self.assertEqual(output, (initial + payload)[-CAPACITY:])

    def test_oversized_write_keeps_tail(self):
        payload = bytes(65 + (i % 26) for i in range(5000))
        total, output = self.write_log(payload)
        self.assertEqual(total, len(payload))
        self.assertEqual(output, payload[-CAPACITY:])

    def test_invalid_magic_does_not_write(self):
        total, output = self.write_log(b"ignored", b"old", magic=0)
        self.assertEqual((total, output), (3, b"old"))


if __name__ == "__main__":
    unittest.main()
