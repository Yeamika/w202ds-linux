#!/usr/bin/env python3
"""Exercise the RAM-retention probe with the pinned LK hook."""
import hashlib
import os
from pathlib import Path
import unittest

from test_watchdog import FACTORY_HASH, WatchdogTest


class RetentionTest(unittest.TestCase):
    run_module = WatchdogTest.run_module

    @classmethod
    def setUpClass(cls):
        cls.module = Path(os.environ["RAMBOOT_RETENTION"]).read_bytes()
        cls.factory = Path(os.environ["RAMBOOT_LK_IMAGE"]).read_bytes()
        if hashlib.sha256(cls.factory).hexdigest() != FACTORY_HASH:
            raise ValueError("LK image is not the pinned B-slot build")

    def test_write_then_watchdog(self):
        state = self.run_module(":init")
        self.assertEqual(state["responses"], ["OKAY"])
        self.assertEqual(state["writes"], [0x1230])
        self.assertEqual(state["starts"], [15000])
        self.assertEqual(state["marker"], (0x5752544E52414D31, 0x8A6D35C27914EFB0,
                                           0x7592CA3D86EB104F))

    def test_valid_and_absent_marker(self):
        self.assertEqual(self.run_module(":probe", retained=True)["responses"], ["OKAY"])
        self.assertEqual(self.run_module(":probe")["responses"], ["FAIL"])

    def test_rejects_invalid_mapping_before_writes(self):
        state = self.run_module(":init", bad_l2=True)
        self.assertEqual(state["responses"], ["FAIL"])
        self.assertEqual((state["writes"], state["starts"], state["marker"]),
                         ([], [], (0, 0, 0)))

    def test_reboot_flag_failure_does_not_write_marker(self):
        state = self.run_module(":init", reject_write=True)
        self.assertEqual(state["responses"], ["FAIL"])
        self.assertEqual(state["writes"], [0x1230, 0x1200])
        self.assertEqual((state["starts"], state["marker"]), ([], (0, 0, 0)))


if __name__ == "__main__":
    unittest.main()
