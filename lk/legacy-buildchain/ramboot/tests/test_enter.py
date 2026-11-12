#!/usr/bin/env python3
"""Exercise the real LK hook and SEC loader; mock only device-side calls."""
import hashlib
import importlib.util
import os
from pathlib import Path
import struct
import subprocess
import unittest
import zlib

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import UC_PROT_EXEC, UC_PROT_READ, UC_PROT_WRITE
from unicorn.arm64_const import UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2

BASE = 0x86000000
FD = 0x82800000
ARG = 0x8A001000
END = 0xB6001000
PTE = 0xB5F01180
PTE_ORIGINAL = 0x0060000086000711
PTE_EXEC = 0x0000000086000791
FACTORY_HASH = "0136f11dcb91d291f163185b189aeaea0bf743bad4034ad3b955e5d6c1ac99ff"


class SecLoaderTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bundle = Path(os.environ["RAMBOOT_ENTER_BUNDLE"]).read_bytes()
        cls.factory = Path(os.environ["RAMBOOT_LK_IMAGE"]).read_bytes()
        cls.fd = Path(os.environ["RAMBOOT_FD"]).read_bytes()
        cls.watchdog_ms = int(os.environ.get("RAMBOOT_WATCHDOG_MS", "30000"))
        if hashlib.sha256(cls.factory).hexdigest() != FACTORY_HASH:
            raise ValueError("LK image is not the pinned B-slot build")
        symbols = subprocess.check_output([os.environ["AARCH64_NM"], "-n",
                                           os.environ["RAMBOOT_ENTER_ELF"]], text=True)
        cls.crc = next(int(line.split()[0], 16) for line in symbols.splitlines()
                       if line.split()[-1] == "crc32")
        spec = importlib.util.spec_from_file_location("mkramboot", os.environ["RAMBOOT_PACKER"])
        packer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(packer)
        cls.entry = FD + packer.sec_entry(cls.fd)

    def run_loader(self, operation, *, bad_crc=False, bad_mapping=False,
                   reject_write=False, bad_copy=False, expect_entry=False, bad_cal=False):
        u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.mem_map(BASE, 0x1000000, UC_PROT_READ | UC_PROT_WRITE)
        u.mem_map(FD, 0x800000)
        u.mem_map(ARG, 0x1000)
        u.mem_map(0xB5000000, 0x1000000)
        u.mem_map(END, 0x1000)
        u.mem_write(0xB5000000, self.factory[512:512 + 0x113860])
        bundle = bytearray(self.bundle)
        if bad_crc:
            bundle[0x10000 + 2048 + 32 + 0x1000] ^= 1
        u.mem_write(BASE, bytes(bundle))
        # A stale RAM image must not supply the missing erased suffix.
        u.mem_write(FD, b'\xa5'*0x800000)
        u.mem_write(ARG, operation.encode() + b"\0")
        u.mem_write(PTE, struct.pack("<Q", PTE_ORIGINAL))
        for i in range(4):
            value = 0x0060000082800711 + (i << 21)
            if bad_mapping and i == 2:
                value = 0
            u.mem_write(0xB5F010A0 + i * 8, struct.pack("<Q", value))
        u.mem_write(0xB50EED8C, struct.pack("<I", 1))
        u.mem_write(0xB50FCCC0, struct.pack("<ii", 694, 0))
        u.mem_write(0xB50FCB98, struct.pack("<5H",1400,3484,200,470,0 if bad_cal else 4))
        u.reg_write(UC_ARM64_REG_SP, 0xB5E00000)
        u.reg_write(UC_ARM64_REG_LR, END)
        u.reg_write(UC_ARM64_REG_X0, ARG)
        u.reg_write(UC_ARM64_REG_X1, BASE)
        u.reg_write(UC_ARM64_REG_X2, len(bundle))
        state = {"monitor": 0x1200, "writes": [], "starts": [], "responses": [],
                 "cleanup": False, "entry": False, "wait": False}
        pending = []

        def string(address):
            chars = bytearray()
            while True:
                char = bytes(u.mem_read(address + len(chars), 1))
                if char == b"\0":
                    return chars.decode("ascii")
                chars.extend(char)

        def external(uc, address, _size, _data):
            if address == 0xB500E11C:
                state["responses"].append(string(uc.reg_read(UC_ARM64_REG_X0)))
            elif address == 0xB506CE54:
                self.assertEqual(uc.reg_read(UC_ARM64_REG_X0), 0x644223AC)
                uc.reg_write(UC_ARM64_REG_X0, state["monitor"])
            elif address == 0xB506CF70:
                value = uc.reg_read(UC_ARM64_REG_X1)
                state["writes"].append(value)
                if not reject_write or value == 0x1200:
                    state["monitor"] = value
                uc.reg_write(UC_ARM64_REG_X0, 0)
            elif address == 0xB5093220:
                state["starts"].append(uc.reg_read(UC_ARM64_REG_X0))
            elif address == 0xB500B904:
                self.assertEqual(state["starts"], [self.watchdog_ms])
                state["cleanup"] = True
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))

        def crc(uc, _address, _size, _data):
            seed, addr, length = (uc.reg_read(reg) for reg in
                                  (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2))
            value = zlib.crc32(bytes(uc.mem_read(addr, length)), seed)
            if bad_copy and addr == FD:
                value ^= 1
            uc.reg_write(UC_ARM64_REG_X0, value)
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))

        def instruction(uc, address, _size, _data):
            insn = struct.unpack("<I", bytes(uc.mem_read(address, 4)))[0]
            code = insn & ~31
            if code == 0xD5384240:
                value = 8
            elif code == 0xD53B0020:
                value = 4 << 16
            elif code == 0xD53C1000:
                value = 0x30C50830 if state["cleanup"] else 0x30C51835
            elif code == 0xD53B4220:
                value = 0x3C0
            elif code == 0xD53BE020:
                value = 1234567  # CNTPCT, deterministic board timing input.
            elif code == 0xD53BE000:
                value = 26000000
            elif code == 0xD50B7A20:
                uc.reg_write(UC_ARM64_REG_PC, address + 4)
                return
            elif insn == 0xD503205F:
                state["wait"] = True
                uc.emu_stop()
                return
            else:
                return
            uc.reg_write(UC_ARM64_REG_X0 + (insn & 31), value)
            uc.reg_write(UC_ARM64_REG_PC, address + 4)

        def pause(uc, _address, _size, _data):
            pending.append(struct.unpack("<Q", bytes(uc.mem_read(PTE, 8)))[0])
            uc.emu_stop()

        def skip(uc, address, _size, _data):
            uc.reg_write(UC_ARM64_REG_PC, address + 4)

        def entry(uc, _address, _size, _data):
            state["entry"] = True
            uc.emu_stop()

        for address in (0xB5004DC0, 0xB500E11C, 0xB5035E94, 0xB506CE54,
                        0xB506CF70, 0xB5093220, 0xB500B904):
            u.hook_add(UC_HOOK_CODE, external, begin=address, end=address)
        u.hook_add(UC_HOOK_CODE, crc, begin=self.crc, end=self.crc)
        u.hook_add(UC_HOOK_CODE, entry, begin=self.entry, end=self.entry)
        for offset in range(0, min(0x10000, len(bundle)) - 3, 4):
            insn = struct.unpack_from("<I", bundle, offset)[0]
            if (insn & ~31 in (0xD5384240, 0xD53B0020, 0xD53C1000,
                               0xD53B4220, 0xD50B7A20, 0xD53BE020, 0xD53BE000) or insn == 0xD503205F):
                u.hook_add(UC_HOOK_CODE, instruction, begin=BASE + offset, end=BASE + offset)
        u.hook_add(UC_HOOK_CODE, pause, begin=0xB500E2A4, end=0xB500E2A4)
        u.hook_add(UC_HOOK_CODE, skip, begin=0xB500E2AC, end=0xB500E2AC)
        pc = 0xB500E1D0
        for _ in range(4):
            u.emu_start(pc, END, count=100000000)
            pc = u.reg_read(UC_ARM64_REG_PC)
            if pc == END or state["entry"] or state["wait"]:
                break
            self.assertEqual(pc, 0xB500E2A4)
            pte = pending.pop()
            self.assertIn(pte, (PTE_ORIGINAL, PTE_EXEC))
            u.mem_protect(BASE, 0x200000, UC_PROT_READ |
                          (UC_PROT_EXEC if pte == PTE_EXEC else UC_PROT_WRITE))
            pc += 4
        if expect_entry:
            self.assertTrue(state["entry"])
        elif not state["wait"]:
            self.assertEqual(pc, END)
            self.assertEqual(struct.unpack("<Q", bytes(u.mem_read(PTE, 8)))[0], PTE_ORIGINAL)
        state["trace"] = struct.unpack("<QQQQQQQQ", bytes(u.mem_read(0x82FFF000, 64)))
        if operation == ':probe' or not state['starts']:
            self.assertEqual(bytes(u.mem_read(FD, 0x800000)), b'\xa5'*0x800000)
        if state["entry"]:
            copied = bytes(u.mem_read(FD, 0x800000))
            trace_offset = 0x7FF000
            record_offset = 0x7FE000
            record = struct.unpack_from('<12I',copied,record_offset)
            self.assertEqual(record[:3],(0x57483232,2,48))
            self.assertEqual(sum(record)&0xffffffff,0)
            self.assertEqual(record[4:7],(3 if bad_cal else 7,694,0))
            self.assertEqual(record[8:],(0,0,0,0) if bad_cal else (1400,3484,200,470))
            self.assertEqual(copied[:min(record_offset,len(self.fd))],
                             self.fd[:record_offset])
            if len(self.fd) < record_offset:
                self.assertEqual(copied[len(self.fd):record_offset],
                                 b'\xa5'*(record_offset-len(self.fd)))
            metrics_offset=record_offset+0x100
            self.assertEqual(copied[record_offset+48:metrics_offset], b'\xff'*(metrics_offset-record_offset-48))
            self.assertEqual(struct.unpack_from('<4Q',copied,metrics_offset),
                             (0x57323032424f4f54,1234567,26000000,0))
            self.assertEqual(copied[metrics_offset+32:trace_offset], b'\xff'*(trace_offset-metrics_offset-32))
            self.assertEqual(copied[trace_offset + 64:], b'\xff'*(0x800000-trace_offset-64))
        return state

    def test_crc_code_against_standard_vector(self):
        u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.mem_map(BASE, 0x10000)
        u.mem_map(ARG, 0x1000)
        u.mem_map(END, 0x1000)
        u.mem_map(0xB5DFF000, 0x1000)
        u.mem_write(BASE, self.bundle[:0x10000])
        u.mem_write(ARG, b"123456789")
        u.reg_write(UC_ARM64_REG_X0, 0)
        u.reg_write(UC_ARM64_REG_X1, ARG)
        u.reg_write(UC_ARM64_REG_X2, 9)
        u.reg_write(UC_ARM64_REG_SP, 0xB5E00000)
        u.reg_write(UC_ARM64_REG_LR, END)
        u.emu_start(self.crc, END, count=10000)
        self.assertEqual(u.reg_read(UC_ARM64_REG_X0), 0xCBF43926)

    def test_probe_is_read_only(self):
        state = self.run_loader(":probe")
        self.assertEqual(state["responses"], ["OKAY"])
        self.assertEqual((state["writes"], state["starts"]), ([], []))

    def test_invalid_image_and_mapping_do_not_arm_watchdog(self):
        for kwargs in ({"bad_crc": True}, {"bad_mapping": True}):
            with self.subTest(**kwargs):
                state = self.run_loader(":init", **kwargs)
                self.assertEqual(state["responses"], ["FAIL"])
                self.assertEqual((state["writes"], state["starts"]), ([], []))

    def test_reboot_flag_failure_does_not_arm_watchdog(self):
        state = self.run_loader(":init", reject_write=True)
        self.assertEqual(state["responses"], ["FAIL"])
        self.assertEqual(state["writes"], [0x1230, 0x1200])
        self.assertEqual(state["starts"], [])

    def test_copied_image_reaches_sec_after_cleanup(self):
        state = self.run_loader(":init", expect_entry=True)
        self.assertEqual(state["writes"], [0x1230])
        self.assertEqual(state["starts"], [self.watchdog_ms])
        self.assertTrue(state["cleanup"])
        self.assertEqual(state["trace"][:5], (0x57323032484E4431, 0x22, 8,
                                              0x30C50830, 0x3C0))
        self.assertEqual(state["trace"][6], self.entry)
        self.assertEqual(state["trace"][7], 0)

    def test_copy_checksum_failure_waits_for_watchdog(self):
        state = self.run_loader(":init", bad_copy=True)
        self.assertTrue(state["wait"])
        self.assertEqual(state["starts"], [self.watchdog_ms])
        self.assertFalse(state["cleanup"])

    def test_uncalibrated_ntc_does_not_invalidate_fgu(self):
        self.run_loader(":init",expect_entry=True,bad_cal=True)


if __name__ == "__main__":
    unittest.main()
