#!/usr/bin/env python3
"""Exercise the temporary handoff and relocated landing code in Unicorn."""
import hashlib
import os
from pathlib import Path
import unittest

from test_watchdog import FACTORY_HASH, WatchdogTest


class HandoffTest(unittest.TestCase):
    run_module = WatchdogTest.run_module

    @classmethod
    def setUpClass(cls):
        cls.module = Path(os.environ["RAMBOOT_HANDOFF"]).read_bytes()
        cls.factory = Path(os.environ["RAMBOOT_LK_IMAGE"]).read_bytes()
        if hashlib.sha256(cls.factory).hexdigest() != FACTORY_HASH:
            raise ValueError("LK image is not the pinned B-slot build")

    def test_native_cleanup_then_relocated_landing(self):
        state = self.run_module(":init", expect_landing=True)
        self.assertEqual(state["writes"], [0x1230])
        self.assertEqual(state["starts"], [15000])
        self.assertEqual(state["cleanups"], 1)
        self.assertEqual(state["trace"][:5], (0x57323032484E4431, 0x22, 8,
                                              0x30C50830, 0x2C0))

    def test_invalid_mapping_does_not_start_handoff(self):
        state = self.run_module(":init", bad_l2=True)
        self.assertEqual(state["responses"], ["FAIL"])
        self.assertEqual((state["writes"], state["starts"], state["cleanups"]), ([], [], 0))

    def test_failed_reboot_flag_never_calls_cleanup(self):
        state = self.run_module(":init", reject_write=True)
        self.assertEqual(state["responses"], ["FAIL"])
        self.assertEqual(state["writes"], [0x1230, 0x1200])
        self.assertEqual((state["starts"], state["cleanups"]), ([], 0))

    def test_trace_reader_handles_wrapped_log(self):
        message = bytes(65 + (index % 26) for index in range(5000))
        state = self.run_module(":probe", trace_log=message)
        self.assertEqual(state["responses"], ["OKAY"])
        self.assertEqual("".join(state["logs"]).encode(), message[-4032:])


if __name__ == "__main__":
    unittest.main()
