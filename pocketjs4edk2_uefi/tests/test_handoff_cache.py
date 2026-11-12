"""Check assembled handoffs against the platform's actual write-back mappings.

Privileged operations are modeled; this verifies ranges, ordering and ABI,
not the behavior of physical caches. Long DC loops are sampled at both ends.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as reg

ROOT = Path(__file__).resolve().parents[1]


class HandoffCacheTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(dir=os.environ['W202_TEST_OUTPUT'])
        cls.out = Path(cls.temp.name)
        edk = Path(os.environ['PJB_EDK2_ROOT'])
        platform = Path(os.environ['W202_PLATFORM_ROOT'])
        cls.includes = ['-I'+str(edk/'ArmPkg/Include'),
                        '-I'+str(edk/'MdePkg/Include'),
                        '-I'+str(edk/'MdePkg/Include/AArch64'),
                        '-I'+str(platform/'Ums9620Pkg/W202ds/Include')]
        wrapper = cls.out/'map.c'
        wrapper.write_text('#include <PiPei.h>\n#define MDE_CPU_AARCH64\n'
            '#include <Library/ArmLib.h>\n#undef MDE_CPU_AARCH64\n'
            '#include <Library/ArmPlatformLib.h>\n#pragma GCC visibility pop\n'
            'extern int printf(const char *, ...);\n'
            '#define W202_LK_TRACE_H\nstatic void LkTraceStage(UINT64 x) {(void)x;}\n'
            '#include "'+str(platform/'Ums9620Pkg/W202ds/Library/LkInheritedPlatformLib/ArmPlatformLibNullMem.c')+'"\n'
            'int main(void) { unsigned i; for(i=0;mMap[i].Length;++i)\n'
            'if(mMap[i].Attributes==ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK)\n'
            'printf("%llx %llx\\n",(unsigned long long)mMap[i].VirtualBase,\n'
            '(unsigned long long)(mMap[i].VirtualBase+mMap[i].Length)); return 0;}\n')
        subprocess.run([os.environ.get('CC', 'cc'), '-fshort-wchar',
            '-I'+str(edk/'MdePkg/Include'), '-I'+str(edk/'MdePkg/Include/X64'),
            '-I'+str(edk/'ArmPkg/Include'), '-I'+str(edk/'ArmPlatformPkg/Include'),
            str(wrapper), '-o', str(cls.out/'map')], check=True)
        cls.mapped = [tuple(int(x, 16) for x in line.split()) for line in
                      subprocess.check_output([str(cls.out/'map')], text=True).splitlines()]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def check_handoff(self, source, symbol, kernel):
        obj = self.out/'handoff.o'
        subprocess.run(['aarch64-linux-gnu-gcc', '-c', '-DASM_PFX(name)=name', *self.includes,
                        str(ROOT/'efi'/source), '-o', str(obj)], check=True)
        # Load the same instructions in either allocator bank, including the
        # high address where the Android regression fault was observed.
        for base in (0x84000000, 0xcb54b000):
            with self.subTest(source=source, base=hex(base)):
                elf = self.out/'handoff.elf'
                binary = self.out/'handoff.bin'
                subprocess.run(['aarch64-linux-gnu-ld', '-Ttext='+hex(base),
                    '-e', symbol, str(obj), '-o', str(elf)], check=True)
                subprocess.run(['aarch64-linux-gnu-objcopy', '-O', 'binary',
                                str(elf), str(binary)], check=True)
                asm = subprocess.check_output(['aarch64-linux-gnu-objdump', '-d', str(elf)], text=True)
                instructions = {int(m[1], 16): (m[2], m[3].split('//')[0].strip())
                    for m in re.finditer(r'^[ \t]*([0-9a-f]+):[ \t]+[0-9a-f]+[ \t]+(\S+)[ \t]*(.*)$', asm, re.M)}
                u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
                u.mem_map(base, 4096)
                u.mem_write(base, binary.read_bytes())
                u.mem_map(0x82fff000, 4096)
                target, argument, stack = 0x86001000, 0x12345678, 0x85f00000
                u.reg_write(reg.UC_ARM64_REG_X0, target)
                u.reg_write(reg.UC_ARM64_REG_X1, argument)
                u.reg_write(reg.UC_ARM64_REG_SP, stack)
                expected = self.mapped if kernel else [r for r in self.mapped if r[0] != 0xd0000000]
                ranges, events = [], []

                def hook(uc, address, size, data):
                    op, args = instructions.get(address, ('', ''))
                    if op == 'mrs':
                        dest, system = [x.strip() for x in args.split(',')]
                        value = 4 << 16 if system == 'ctr_el0' else 0x1005
                        uc.reg_write(getattr(reg, 'UC_ARM64_REG_'+dest.upper()), value)
                    elif op == 'dc':
                        start = uc.reg_read(reg.UC_ARM64_REG_X6)
                        end = uc.reg_read(reg.UC_ARM64_REG_X7)
                        stride = uc.reg_read(reg.UC_ARM64_REG_X5)
                        self.assertEqual(stride, 64)
                        self.assertLess(start, end)
                        self.assertEqual((end-start) % stride, 0)
                        ranges.append((start, end))
                        # Execute the final add/cmp/branch, with the final
                        # cache-line operation sampled in place of the middle.
                        uc.reg_write(reg.UC_ARM64_REG_X6, end-stride)
                    elif op == 'msr' and args.startswith('sctlr_el2'):
                        self.assertEqual(ranges, expected)
                        self.assertIn('ic', events)
                        value = uc.reg_read(reg.UC_ARM64_REG_X4)
                        self.assertEqual(value & 0x1005, 0)
                    elif op == 'br':
                        self.assertEqual(uc.reg_read(reg.UC_ARM64_REG_X19), target)
                        self.assertEqual(uc.reg_read(reg.UC_ARM64_REG_X0), argument)
                        self.assertEqual(uc.reg_read(reg.UC_ARM64_REG_SP), stack)
                        self.assertEqual(ranges, expected)
                        self.assertIn('tlbi', events)
                        events.append('jump')
                        uc.emu_stop()
                        return
                    elif op not in ('msr', 'ic', 'tlbi'):
                        return
                    events.append(op)
                    uc.reg_write(reg.UC_ARM64_REG_PC, address+4)

                u.hook_add(UC_HOOK_CODE, hook)
                u.emu_start(base, base+4096, count=256)
                self.assertEqual(events[-1], 'jump')

    def test_android_lk(self):
        self.check_handoff('Android/AndroidLk/ReturnLk.S', 'W202ReturnLk', False)

    def test_android_kernel(self):
        self.check_handoff('Android/AndroidBoot/EnterKernel.S', 'W202EnterKernel', True)

    def test_linux_kernel(self):
        self.check_handoff('LinuxBoot/EnterKernel.S', 'W202EnterKernel', True)


if __name__ == '__main__':
    unittest.main()
