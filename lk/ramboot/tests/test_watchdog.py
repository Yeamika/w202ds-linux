#!/usr/bin/env python3
"""Run the watchdog probe through the pinned LK hook with mocked PMIC calls."""
import hashlib
import os
from pathlib import Path
import struct
import unittest

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import UC_PROT_EXEC, UC_PROT_READ, UC_PROT_WRITE
from unicorn.arm64_const import UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2

BASE = 0x86000000
HOOK = 0xB500E1D0
PTE = 0xB5F01180
PTE_ORIGINAL = 0x0060000086000711
PTE_EXEC = 0x0000000086000791
ARG = 0x8A001000
END = 0xB6001000
FACTORY_HASH = "0136f11dcb91d291f163185b189aeaea0bf743bad4034ad3b955e5d6c1ac99ff"


class WatchdogTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = Path(os.environ["RAMBOOT_WATCHDOG"]).read_bytes()
        cls.factory = Path(os.environ["RAMBOOT_LK_IMAGE"]).read_bytes()
        if hashlib.sha256(cls.factory).hexdigest() != FACTORY_HASH:
            raise ValueError("LK image is not the pinned B-slot build")
        if struct.unpack_from("<I", cls.module, 4)[0] != len(cls.module):
            raise ValueError("PLK1 length mismatch")

    def run_module(self, operation, *, el=8, cpus=1, bad_opcode=False,
                   reject_write=False, retained=False, bad_l2=False,
                   expect_landing=False, trace_log=None):
        u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.mem_map(BASE, 0x200000, UC_PROT_READ | UC_PROT_WRITE)
        u.mem_map(ARG, 0x1000)
        u.mem_map(0x82800000, 0x200000)
        u.mem_map(0x82E00000, 0x200000)
        u.mem_map(0xB5000000, 0x1000000)
        u.mem_map(END, 0x1000)
        u.mem_write(0xB5000000, self.factory[512:512 + 0x113860])
        u.mem_write(BASE, self.module)
        u.mem_write(ARG, operation.encode() + b"\0")
        u.mem_write(PTE, struct.pack("<Q", PTE_ORIGINAL))
        u.mem_write(0xB5F010A0, struct.pack("<Q", 0x0060000082800711))
        u.mem_write(0xB5F010B8, struct.pack("<Q", 0 if bad_l2 else 0x0060000082E00711))
        u.mem_write(0xB50EED8C, struct.pack("<I", cpus))
        if retained:
            u.mem_write(0x82FFF000, struct.pack("<QQQ", 0x5752544E52414D31,
                                                0x8A6D35C27914EFB0,
                                                0x7592CA3D86EB104F))
        if trace_log is not None:
            ring = bytearray(4032)
            for index, byte in enumerate(trace_log):
                ring[index % len(ring)] = byte
            u.mem_write(0x82FFF000, struct.pack("<QQQQQQQQ", 0x57323032484E4431,
                                                0x35, 8, 0, 0, 0, 0,
                                                len(trace_log)) + ring)
        if bad_opcode:
            u.mem_write(0xB5093224, b"\0\0\0\0")
        u.reg_write(UC_ARM64_REG_SP, 0xB5E00000)
        u.reg_write(UC_ARM64_REG_LR, END)
        u.reg_write(UC_ARM64_REG_X0, ARG)
        u.reg_write(UC_ARM64_REG_X1, BASE)
        u.reg_write(UC_ARM64_REG_X2, len(self.module))

        state = {"monitor": 0x1200, "writes": [], "starts": [], "responses": [], "logs": [],
                 "cleanups": 0, "landed": False}
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
            elif address == 0xB5035E94:
                if string(uc.reg_read(UC_ARM64_REG_X0)) == "EDK2-RAM: %s":
                    state["logs"].append(string(uc.reg_read(UC_ARM64_REG_X1)))
            elif address == 0xB506CE54:
                self.assertEqual(uc.reg_read(UC_ARM64_REG_X0), 0x644223AC)
                uc.reg_write(UC_ARM64_REG_X0, state["monitor"])
            elif address == 0xB506CF70:
                self.assertEqual(uc.reg_read(UC_ARM64_REG_X0), 0x644223AC)
                value = uc.reg_read(UC_ARM64_REG_X1)
                state["writes"].append(value)
                if not reject_write or value == 0x1200:
                    state["monitor"] = value
                uc.reg_write(UC_ARM64_REG_X0, 0)
            elif address == 0xB5093220:
                state["starts"].append(uc.reg_read(UC_ARM64_REG_X0))
            elif address == 0xB500B904:
                self.assertEqual(state["starts"], [15000])
                state["cleanups"] += 1
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))

        def instruction(uc, address, _size, _data):
            insn = struct.unpack("<I", bytes(uc.mem_read(address, 4)))[0]
            if insn & ~31 == 0xD5384240:
                uc.reg_write(UC_ARM64_REG_X0 + (insn & 31), el)
                uc.reg_write(UC_ARM64_REG_PC, address + 4)
            elif insn & ~31 == 0xD53B0020:
                uc.reg_write(UC_ARM64_REG_X0 + (insn & 31), 4 << 16)
                uc.reg_write(UC_ARM64_REG_PC, address + 4)
            elif insn & ~31 == 0xD53C1000:
                value = 0x30C50830 if state["cleanups"] else 0x30C51835
                uc.reg_write(UC_ARM64_REG_X0 + (insn & 31), value)
                uc.reg_write(UC_ARM64_REG_PC, address + 4)
            elif insn & ~31 == 0xD53B4220:
                uc.reg_write(UC_ARM64_REG_X0 + (insn & 31), 0x2C0)
                uc.reg_write(UC_ARM64_REG_PC, address + 4)
            elif insn & ~31 in (0xD50B7A20, 0xD5087620):
                uc.reg_write(UC_ARM64_REG_PC, address + 4)
            elif insn == 0xD503205F and expect_landing:
                state["landed"] = True
                uc.emu_stop()

        def pause(uc, _address, _size, _data):
            pending.append(struct.unpack("<Q", bytes(uc.mem_read(PTE, 8)))[0])
            uc.emu_stop()

        def skip(uc, address, _size, _data):
            uc.reg_write(UC_ARM64_REG_PC, address + 4)

        for address in (0xB5004DC0, 0xB500E11C, 0xB5035E94,
                        0xB506CE54, 0xB506CF70, 0xB5093220, 0xB500B904):
            u.hook_add(UC_HOOK_CODE, external, begin=address, end=address)
        u.hook_add(UC_HOOK_CODE, instruction, begin=BASE, end=BASE + len(self.module) - 1)
        u.hook_add(UC_HOOK_CODE, instruction, begin=0x82800000, end=0x82800FFF)
        u.hook_add(UC_HOOK_CODE, pause, begin=0xB500E2A4, end=0xB500E2A4)
        u.hook_add(UC_HOOK_CODE, skip, begin=0xB500E2AC, end=0xB500E2AC)

        pc = HOOK
        for _ in range(4):
            u.emu_start(pc, END, count=1000000)
            pc = u.reg_read(UC_ARM64_REG_PC)
            if state["landed"]:
                break
            if pc == END:
                break
            self.assertEqual(pc, 0xB500E2A4)
            pte = pending.pop()
            self.assertIn(pte, (PTE_ORIGINAL, PTE_EXEC))
            u.mem_protect(BASE, 0x200000, UC_PROT_READ |
                          (UC_PROT_EXEC if pte == PTE_EXEC else UC_PROT_WRITE))
            pc += 4
        if expect_landing:
            self.assertTrue(state["landed"])
        else:
            self.assertEqual(pc, END)
            self.assertEqual(struct.unpack("<Q", bytes(u.mem_read(PTE, 8)))[0], PTE_ORIGINAL)
        state["marker"] = struct.unpack("<QQQ", bytes(u.mem_read(0x82FFF000, 24)))
        state["trace"] = struct.unpack("<QQQQQQ", bytes(u.mem_read(0x82FFF000, 48)))
        return state

    def test_probe_is_read_only(self):
        state = self.run_module(":probe")
        self.assertEqual(state["responses"], ["OKAY"])
        self.assertEqual((state["writes"], state["starts"]), ([], []))

    def test_init_sets_normal_reason_then_arms_watchdog(self):
        state = self.run_module(":init")
        self.assertEqual(state["responses"], ["OKAY"])
        self.assertEqual(state["writes"], [0x1240])
        self.assertEqual(state["starts"], [15000])

    def test_rejected_write_restores_monitor_without_arming(self):
        state = self.run_module(":init", reject_write=True)
        self.assertEqual(state["responses"], ["FAIL"])
        self.assertEqual(state["writes"], [0x1240, 0x1200])
        self.assertEqual(state["starts"], [])

    def test_incompatible_entry_state_is_read_only(self):
        for kwargs in ({"el": 4}, {"cpus": 2}, {"bad_opcode": True}):
            with self.subTest(**kwargs):
                state = self.run_module(":init", **kwargs)
                self.assertEqual(state["responses"], ["FAIL"])
                self.assertEqual((state["writes"], state["starts"]), ([], []))

    def test_other_operation_is_rejected(self):
        state = self.run_module(":frame")
        self.assertEqual(state["responses"], ["FAIL"])
        self.assertEqual((state["writes"], state["starts"]), ([], []))


if __name__ == "__main__":
    unittest.main()
