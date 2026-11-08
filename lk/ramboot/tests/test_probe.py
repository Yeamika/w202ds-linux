#!/usr/bin/env python3
"""Exercise the compiled probe through the pinned LK hook in Unicorn."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import unittest
import zlib

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import UC_PROT_EXEC, UC_PROT_READ, UC_PROT_WRITE
from unicorn.arm64_const import *

BASE = 0x86000000
HOOK = 0xb500e1d0
PTE = 0xb5f01180
PTE_ORIGINAL = 0x0060000086000711
PTE_EXEC = 0x0000000086000791
STACK = 0xb5e00000
ARG = 0x8a001000
END = 0xb6001000
FACTORY_HASH = '0136f11dcb91d291f163185b189aeaea0bf743bad4034ad3b955e5d6c1ac99ff'


class ProbeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.probe = Path(os.environ['RAMBOOT_PROBE']).read_bytes()
        cls.factory = Path(os.environ['RAMBOOT_LK_IMAGE']).read_bytes()
        cls.bundle = Path(os.environ['RAMBOOT_CHECK_BUNDLE']).read_bytes()
        cls.fd = Path(os.environ['RAMBOOT_FD']).read_bytes()
        symbols = subprocess.check_output([os.environ['AARCH64_NM'], '-n',
                                           os.environ['RAMBOOT_CHECK_ELF']], text=True)
        cls.crc_address = next(int(line.split()[0], 16) for line in symbols.splitlines()
                               if line.split()[-1] == 'crc32')
        if hashlib.sha256(cls.factory).hexdigest() != FACTORY_HASH:
            raise ValueError('The LK backup is not the pinned resident image')
        if struct.unpack_from('<I', cls.probe, 4)[0] != len(cls.probe):
            raise ValueError('PLK1 header length differs from the payload')

    def run_hook(self, operation, *, el=8, corrupt_header=False, size=None,
                 payload=None, mock_crc=False):
        u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.mem_map(BASE, 0x1000000, UC_PROT_READ | UC_PROT_WRITE)
        u.mem_map(ARG, 0x1000)
        u.mem_map(0xb5000000, 0x1000000)
        u.mem_map(END, 0x1000)
        u.mem_write(0xb5000000, self.factory[512:512 + 0x113860])
        probe = bytearray(self.probe if payload is None else payload)
        if corrupt_header:
            probe[8:12] = b'BAD!'
        u.mem_write(BASE, bytes(probe))
        u.mem_write(ARG, operation.encode() + b'\0')
        u.mem_write(PTE, struct.pack('<Q', PTE_ORIGINAL))
        u.mem_write(0xb50eed8c, struct.pack('<I', 2))
        for offset, value in ((0x80, 0x82000711), (0xa0, 0x82800711),
                              (0x178, 0x85e00711)):
            u.mem_write(0xb5f01000 + offset, struct.pack('<Q', value))
        responses, logs, transitions, system_reads = [], [], [], []
        pending = []
        initial = {UC_ARM64_REG_X18: 0x1018, UC_ARM64_REG_X29: 0x1029,
                   **{UC_ARM64_REG_X19 + i: 0x1100 + i for i in range(10)}}
        for reg, value in initial.items():
            u.reg_write(reg, value)
        u.reg_write(UC_ARM64_REG_SP, STACK)
        u.reg_write(UC_ARM64_REG_LR, END)
        u.reg_write(UC_ARM64_REG_X0, ARG)
        u.reg_write(UC_ARM64_REG_X1, BASE)
        u.reg_write(UC_ARM64_REG_X2, len(probe) if size is None else size)

        def string(address):
            out = bytearray()
            for i in range(256):
                char = bytes(u.mem_read(address + i, 1))
                if char == b'\0':
                    return out.decode('ascii')
                out += char
            raise AssertionError('unterminated string')

        def external(uc, address, _size, _context):
            if address == 0xb500e11c:
                responses.append(string(uc.reg_read(UC_ARM64_REG_X0)))
            elif address == 0xb5035e94:
                logs.append((string(uc.reg_read(UC_ARM64_REG_X0)),
                             [uc.reg_read(UC_ARM64_REG_X1 + i) for i in range(4)]))
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))

        def code(uc, address, _size, _context):
            insn = struct.unpack('<I', bytes(uc.mem_read(address, 4)))[0]
            registers = {
                0xd5384240: ('CurrentEL', el),
                0xd53800a0: ('MPIDR', 0),
                0xd53b4220: ('DAIF', 0x3c0),
                0xd53c1000: ('SCTLR_EL2', 0x1005),
                0xd53c2040: ('TCR_EL2', 0x123),
                0xd53c2000: ('TTBR0_EL2', 0xb5f00000),
            }
            key = insn & ~31
            if key in registers:
                name, value = registers[key]
                system_reads.append(name)
                uc.reg_write(UC_ARM64_REG_X0 + (insn & 31), value)
                uc.reg_write(UC_ARM64_REG_PC, address + 4)

        def pause_for_pte(uc, _address, _size, _context):
            pending.append(struct.unpack('<Q', bytes(uc.mem_read(PTE, 8)))[0])
            uc.emu_stop()

        def skip_tlbi(uc, address, _size, _context):
            uc.reg_write(UC_ARM64_REG_PC, address + 4)

        def crc(uc, _address, _size, _context):
            seed = uc.reg_read(UC_ARM64_REG_X0)
            src = uc.reg_read(UC_ARM64_REG_X1)
            length = uc.reg_read(UC_ARM64_REG_X2)
            uc.reg_write(UC_ARM64_REG_X0, zlib.crc32(bytes(uc.mem_read(src, length)), seed))
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))

        for address in (0xb5004dc0, 0xb500e11c, 0xb5035e94):
            u.hook_add(UC_HOOK_CODE, external, begin=address, end=address)
        u.hook_add(UC_HOOK_CODE, code, begin=BASE, end=BASE + len(probe) - 1)
        if mock_crc:
            u.hook_add(UC_HOOK_CODE, crc, begin=self.crc_address, end=self.crc_address)
        u.hook_add(UC_HOOK_CODE, pause_for_pte, begin=0xb500e2a4, end=0xb500e2a4)
        u.hook_add(UC_HOOK_CODE, skip_tlbi, begin=0xb500e2ac, end=0xb500e2ac)

        pc = HOOK
        for _ in range(4):
            u.emu_start(pc, END, count=1000000)
            pc = u.reg_read(UC_ARM64_REG_PC)
            if pc == END:
                break
            self.assertEqual(pc, 0xb500e2a4)
            pte = pending.pop()
            self.assertIn(pte, (PTE_ORIGINAL, PTE_EXEC))
            transitions.append(pte)
            u.mem_protect(BASE, 0x200000, UC_PROT_READ |
                          (UC_PROT_EXEC if pte == PTE_EXEC else UC_PROT_WRITE))
            pc += 4
        self.assertEqual(pc, END)
        self.assertEqual(u.reg_read(UC_ARM64_REG_SP), STACK)
        for reg, value in initial.items():
            self.assertEqual(u.reg_read(reg), value)
        self.assertEqual(struct.unpack('<Q', bytes(u.mem_read(PTE, 8)))[0], PTE_ORIGINAL)
        return responses, logs, transitions, system_reads

    def test_real_hook_and_probe(self):
        responses, logs, transitions, reads = self.run_hook(':probe')
        self.assertEqual(responses, ['OKAY'])
        self.assertEqual(transitions, [PTE_EXEC, PTE_ORIGINAL])
        self.assertEqual(reads, ['CurrentEL', 'MPIDR', 'DAIF', 'SCTLR_EL2', 'TCR_EL2', 'TTBR0_EL2'])
        self.assertEqual(len(logs), 4)
        self.assertEqual(logs[2][1][0], 2)
        self.assertEqual(logs[3][1], [0x82000711, 0x82800711, 0x85e00711, PTE_EXEC])

    def test_rejects_unsupported_operation(self):
        responses, logs, transitions, reads = self.run_hook(':init')
        self.assertEqual((responses, logs, reads), (['FAIL'], [], []))
        self.assertEqual(transitions, [PTE_EXEC, PTE_ORIGINAL])

    def test_rejects_unexpected_el_before_el2_registers(self):
        responses, logs, transitions, reads = self.run_hook(':probe', el=4)
        self.assertEqual(responses, ['FAIL'])
        self.assertEqual(len(logs), 1)
        self.assertEqual(reads, ['CurrentEL', 'MPIDR', 'DAIF'])
        self.assertEqual(transitions, [PTE_EXEC, PTE_ORIGINAL])

    def test_hook_rejects_wrong_header_and_length(self):
        for options in ({'corrupt_header': True}, {'size': len(self.probe) - 1}):
            with self.subTest(options=options):
                responses, logs, transitions, reads = self.run_hook(':probe', **options)
                self.assertEqual((responses, logs, transitions, reads), (['FAIL'], [], [], []))

    def test_actual_crc_code(self):
        u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.mem_map(BASE, 0x200000)
        u.mem_map(ARG, 0x1000)
        u.mem_map(STACK - 0x1000, 0x1000)
        u.mem_map(END, 0x1000)
        u.mem_write(BASE, self.bundle[:0x10000])
        u.mem_write(ARG, b'123456789')
        u.reg_write(UC_ARM64_REG_X0, 0)
        u.reg_write(UC_ARM64_REG_X1, ARG)
        u.reg_write(UC_ARM64_REG_X2, 9)
        u.reg_write(UC_ARM64_REG_SP, STACK)
        u.reg_write(UC_ARM64_REG_LR, END)
        u.emu_start(self.crc_address, END, count=10000)
        self.assertEqual(u.reg_read(UC_ARM64_REG_X0), 0xcbf43926)

    def test_real_preflight_accepts_bundle(self):
        responses, logs, transitions, _ = self.run_hook(':probe', payload=self.bundle,
                                                         mock_crc=True)
        self.assertEqual(responses, ['OKAY'])
        self.assertEqual(transitions, [PTE_EXEC, PTE_ORIGINAL])
        self.assertEqual(len(logs), 2)
        self.assertEqual(logs[0][1][0], len(self.fd))
        self.assertTrue(0x82800000 <= logs[0][1][1] < 0x82800000+len(self.fd))
        self.assertIn('SEC entry was not called', logs[1][0])

    def test_preflight_rejects_bad_crc_and_run_command(self):
        corrupt = bytearray(self.bundle)
        corrupt[-2049] ^= 1
        for operation, payload in ((':probe', corrupt), (':init', self.bundle)):
            with self.subTest(operation=operation):
                responses, _, transitions, _ = self.run_hook(operation, payload=payload,
                                                              mock_crc=True)
                self.assertEqual(responses, ['FAIL'])
                self.assertEqual(transitions, [PTE_EXEC, PTE_ORIGINAL])


if __name__ == '__main__':
    unittest.main()
