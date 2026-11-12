#!/usr/bin/env python3
"""Execute the packaged ARM64 native filter, storage loader and SEC shim."""
import os
from pathlib import Path
import struct
import subprocess
import unittest
import zlib
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import UC_PROT_READ, UC_PROT_WRITE, UC_PROT_EXEC
from unicorn.arm64_const import *

BASE, FD, END = 0x86000000, 0x82800000, 0xb6001000
PTE, ORIGINAL, EXEC = 0xb5f01180, 0x0060000086000711, 0x0000000086000791
HANDLERS = {2:0xb500da88, 16:0xb500da88, 4:0xb500db88,
            5:0xb500ddf0, 7:0xb500ded0, 3:0xb500dc80}


class ResidentTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.flash = Path(os.environ['RESIDENT_IMAGE']).read_bytes()
        cls.fd = Path(os.environ['RAMBOOT_FD']).read_bytes()
        symbols = subprocess.check_output([os.environ['AARCH64_NM'], '-n',
                                           os.environ['RESIDENT_ELF']], text=True)
        cls.crc = next(int(line.split()[0],16) for line in symbols.splitlines()
                       if line.split()[-1]=='crc32')
        cls.return_resume = next((int(line.split()[0],16) for line in symbols.splitlines()
                                  if line.split()[-1]=='lk_return_resume'), None)
        cls.sec = FD + int(os.environ.get('RAMBOOT_SEC_OFFSET','0x6e78'),0)
        cls.watchdog_ms = int(os.environ.get('RAMBOOT_WATCHDOG_MS','180000'))
        cls.preflight = os.environ.get('RESIDENT_PREFLIGHT') == '1'

    def run_mode(self, mode, *, corrupt=False, io_fail=False, lcd_fail=False,
                 bad_panel=False, bad_pte=False, handler=None, two_cpus=False,
                 flip_fail=False, bad_scanout=False, reject_key_input=False):
        u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.mem_map(BASE, 0x1000000, UC_PROT_READ|UC_PROT_WRITE)
        u.mem_map(FD, 0x800000)
        u.mem_map(0xb5000000, 0x1000000)
        u.mem_map(END, 0x1000)
        u.mem_map(0x31000000,0x2000)
        # The production loader enables LK_RETURN. Execute its real GIC save
        # code against a quiet, affinity-routed 64-IRQ controller model.
        u.mem_map(0x12000000,0x80000)
        u.mem_write(0x12000000,struct.pack('<II',0x10,1))
        u.mem_write(0xb5000000, self.flash[512:512+0x113860])
        u.mem_write(FD, b'\xa5'*0x800000)
        u.mem_write(PTE,struct.pack('<Q',0 if bad_pte else ORIGINAL))
        for i in range(4):
            u.mem_write(0xb5f010a0+i*8,struct.pack('<Q',0x0060000082800711+(i<<21)))
        u.mem_write(0xb50eed8c,struct.pack('<I',2 if two_cpus else 1))
        u.mem_write(0xb50fccc0,struct.pack('<ii',694,0))
        u.mem_write(0xb50fcb98,struct.pack('<5H',1400,3484,200,470,4))
        u.mem_write(0xb56bc00a,struct.pack('<HH',1200,1920))
        u.mem_write(0xb56bb088,struct.pack('<Q',0xb0000000))
        u.mem_write(0xb50ffc9e,b'\x08')
        # Cold boot has a probed controller but no inherited scanout layer.
        for offset,value in ((0,0x600),(0x18,0x078004b0)):
            u.mem_write(0x31000000+offset,struct.pack('<I',value))
        native = HANDLERS[mode] if handler is None else handler
        u.reg_write(UC_ARM64_REG_X0,native)
        u.reg_write(UC_ARM64_REG_X19,mode)
        u.reg_write(UC_ARM64_REG_SP,0xb5e00000)
        u.reg_write(UC_ARM64_REG_LR,END)
        storage = bytearray(self.flash)
        state = dict(reads=[],writes=[],watchdogs=[],display=[],native=[],entry=False,fastboot=False,
                     cleanup=False,pmic={0x644223ac:0x1200,0x64420284:0x0b})

        def ret(value=None):
            if value is not None: u.reg_write(UC_ARM64_REG_X0,value&0xffffffffffffffff)
            u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))

        def hardware(uc,pc,size,data):
            x0,x1,x2,x3 = (uc.reg_read(reg) for reg in
                           [UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3])
            if pc==0xb50405ec:
                self.assertEqual(bytes(uc.mem_read(x0,8)),b'uboot_b\0')
                state['reads'].append((x1,x2,x3))
                payload=bytearray(storage[x2:x2+x1])
                self.assertEqual(x2,0x228000)
                self.assertEqual(x3,BASE)
                if corrupt: payload[0x11000]^=1
                if not io_fail: uc.mem_write(x3,bytes(payload))
                ret(-1 if io_fail else 0)
            elif pc==0xb503f5d0:
                self.fail('resident boot attempted a storage write')
            elif pc==0xb50932a0:
                # Execute the real pinned stop_watchdog body. This is not a
                # board-setup no-op: it clears the PMIC counter-enable bit.
                state['display'].append('stop-watchdog')
            elif pc==0xb5086e5c:
                self.assertEqual(state['pmic'].get(0x64420048,0)&0xa,0xa,
                                 'panel initialization has no live reset watchdog')
                self.assertEqual(state['pmic'].get(0x64422008,0)&4,4)
                self.assertEqual(state['pmic'].get(0x64422010,0)&4,4)
                reload=(state['pmic'].get(0x64420044,0)<<16)|state['pmic'].get(0x64420040,0)
                self.assertEqual(reload,self.watchdog_ms*32768//1000)
                state['display'].append('lcd');ret(-1 if lcd_fail else 0)
            elif pc==0xb5089bfc: ret(0xdeaddeaddeaddead if bad_panel else 0xb56bc000)
            elif pc==0xb508868c:
                # Run the actual native lcd_enable body; model only its
                # allocator and the display hardware reached by the flip.
                self.assertEqual(state['pmic'].get(0x64420048,0)&0xa,0xa)
                state['display'].append('flip')
            elif pc==0xb5033434:
                self.assertEqual(x0,92);ret(0xb5700000)
            elif pc==0xb50335a4: ret()
            elif pc==0xb5089168:
                self.assertEqual(x0,0xb56bc0b0)
                config=bytes(uc.mem_read(x1,92))
                self.assertEqual(struct.unpack_from('<I',config,0)[0],1)
                self.assertEqual(struct.unpack_from('<I',config,12)[0],0xb0000000)
                self.assertEqual(struct.unpack_from('<I',config,28)[0],2400)
                self.assertEqual(struct.unpack_from('<HH',config,64),(1200,1920))
                self.assertEqual(struct.unpack_from('<I',config,68)[0],0x36314752)
                if not flip_fail:
                    for offset,value in ((0x2c,1),(0x30,0 if bad_scanout else 0xb0000000),
                                         (0x40,0x00200041),(0x4c,1200)):
                        uc.mem_write(0x31000000+offset,struct.pack('<I',value))
                ret()
            elif pc==0xb50035a4: state['display'].append(('backlight',x0));ret()
            elif pc==0xb506ce54: ret(state['pmic'].get(x0,0))
            elif pc==0xb506cf70:
                if x0!=0x64420284 or not reject_key_input:
                    state['pmic'][x0]=x1&0xffff
                if x0==0x644223ac: state['writes'].append(x1)
                ret(0)
            elif pc==0xb506d0dc:
                state['pmic'][x0]=((state['pmic'].get(x0,0)&~x2)|x1)&0xffff
                ret(0)
            elif pc==0xb5093220:
                # Execute the real start_watchdog/wdt_config body, mocking
                # only the ADI bus so reset enable and reload are observable.
                state['watchdogs'].append(x0)
            elif pc==0xb500b904:
                self.assertEqual(state['pmic'].get(0x64420048,0)&0xa,0xa,
                                 'native cleanup has no live reset watchdog')
                state['cleanup']=True;ret()
            elif pc in (0xb5004dc0,0xb5035e94,0xb5035c78,0xb500e11c): ret()
            elif pc==0xb5012a80: state['fastboot']=True;ret()
            elif pc==native: state['native'].append(pc);ret()
            elif pc==0xb5006674: uc.reg_write(UC_ARM64_REG_PC,END)

        for addr in set([0xb50405ec,0xb503f5d0,0xb50932a0,0xb5086e5c,0xb5089bfc,0xb50035a4,
                         0xb508868c,0xb5033434,0xb50335a4,0xb5089168,
                         0xb506ce54,0xb506cf70,0xb506d0dc,0xb5093220,0xb500b904,0xb5004dc0,
                         0xb5035e94,0xb5035c78,0xb500e11c,0xb5006674,0xb5012a80,native]):
            u.hook_add(UC_HOOK_CODE,hardware,begin=addr,end=addr)
        def crc(uc,pc,size,data):
            seed,addr,n=(uc.reg_read(r) for r in [UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2])
            ret(zlib.crc32(bytes(uc.mem_read(addr,n)),seed))
        u.hook_add(UC_HOOK_CODE,crc,begin=self.crc,end=self.crc)
        def entry(uc,pc,size,data): state['entry']=True;uc.emu_stop()
        u.hook_add(UC_HOOK_CODE,entry,begin=self.sec,end=self.sec)
        def system(uc,pc,size,data):
            ins=struct.unpack('<I',bytes(uc.mem_read(pc,4)))[0];code=ins&~31
            vals={0xd5384240:8,0xd53b0020:4<<16,0xd53c1000:0x30c50830,
                  0xd53b4220:0x3c0,0xd53be020:1234567,0xd53be000:26000000,
                  0xd5384200:1} # SPSel=1; MPIDR and other saved registers are zero.
            if ins&0xfff00000==0xd5300000:
                uc.reg_write(UC_ARM64_REG_X0+(ins&31),vals.get(code,0))
                uc.reg_write(UC_ARM64_REG_PC,pc+4)
            elif code==0xd50b7a20 or ins==0xd5034fdf:
                uc.reg_write(UC_ARM64_REG_PC,pc+4)
        bundle=self.flash[0x228000:0x238000]
        for off in range(0,len(bundle)-3,4):
            ins=struct.unpack_from('<I',bundle,off)[0]
            if ins&0xfff00000==0xd5300000 or ins&~31==0xd50b7a20 or ins==0xd5034fdf:
                u.hook_add(UC_HOOK_CODE,system,begin=BASE+off,end=BASE+off)
        def pause(uc,pc,size,data): uc.emu_stop()
        def skip(uc,pc,size,data): uc.reg_write(UC_ARM64_REG_PC,pc+4)
        u.hook_add(UC_HOOK_CODE,pause,begin=0xb500e2a4,end=0xb500e2a4)
        u.hook_add(UC_HOOK_CODE,skip,begin=0xb500e2ac,end=0xb500e2ac)
        pc=0xb5006670
        for _ in range(4):
            u.emu_start(pc,END,count=100000000)
            pc=u.reg_read(UC_ARM64_REG_PC)
            if pc==END or state['entry']: break
            self.assertEqual(pc,0xb500e2a4)
            pte=struct.unpack('<Q',bytes(u.mem_read(PTE,8)))[0]
            self.assertIn(pte,(ORIGINAL,EXEC))
            u.mem_protect(BASE,0x200000,UC_PROT_READ|(UC_PROT_EXEC if pte==EXEC else UC_PROT_WRITE))
            pc+=4
        if state['entry']:
            self.assertTrue(state['cleanup'])
            self.assertIn('flip',state['display'])
            self.assertEqual(state['pmic'][0x64420284],0x1b)
            self.assertEqual(struct.unpack('<I',bytes(u.mem_read(0x31000030,4)))[0],0xb0000000)
            self.assertEqual(bytes(u.mem_read(FD,len(self.fd))),self.fd)
            record=struct.unpack('<12I',bytes(u.mem_read(0x82ffe000,48)))
            self.assertEqual(record[4],7 if self.preflight else 15)
            self.assertEqual(sum(record)&0xffffffff,0)
            if self.return_resume is not None:
                descriptor=struct.unpack('<10I',bytes(u.mem_read(0x82ffe200,40)))
                self.assertEqual(descriptor[:3],(0x4c4b5231,1,40))
                self.assertEqual(descriptor[4:8],(self.return_resume,0,0x86400000,0))
                self.assertEqual(descriptor[9],3)
                self.assertEqual(sum(descriptor)&0xffffffff,0)
            self.assertEqual(state['watchdogs'],[self.watchdog_ms]*2)
        else:
            self.assertEqual(pc,END)
            if mode in (2,16):
                self.assertTrue(state['fastboot'])
                self.assertEqual(state['native'],[])
            else:
                self.assertFalse(state['fastboot'])
                self.assertEqual(state['native'],[native])
            if state['display']:
                self.assertEqual(state['watchdogs'],[self.watchdog_ms])
                self.assertEqual(state['writes'],[0x1230])
            else:
                self.assertFalse(state['watchdogs'])
                self.assertFalse(state['writes'])
            self.assertEqual(bytes(u.mem_read(FD,0x800000)),b'\xa5'*0x800000)
            self.assertEqual(u.reg_read(UC_ARM64_REG_SP),0xb5e00000)
            self.assertEqual(u.reg_read(UC_ARM64_REG_X19),mode)
        return state

    def test_normal_and_external_reset_reach_sec(self):
        for mode in (2,16):
            with self.subTest(mode=mode): self.assertTrue(self.run_mode(mode)['entry'])

    def test_recovery_fastboot_charging_and_other_modes_stay_native(self):
        for mode in (4,5,7,3):
            with self.subTest(mode=mode):
                state=self.run_mode(mode)
                self.assertFalse(state['entry'])
                self.assertFalse(state['display'])
                self.assertFalse(state['reads'])

    def test_corruption_read_failure_display_and_cpu_failure_preserve_native_path(self):
        for kwargs in [dict(corrupt=True),dict(io_fail=True),dict(lcd_fail=True),
                       dict(bad_panel=True),dict(bad_pte=True),dict(two_cpus=True),
                       dict(handler=0xb500db88)]:
            with self.subTest(**kwargs): self.assertFalse(self.run_mode(2,**kwargs)['entry'])

    def test_unconfigured_scanout_or_volume_input_never_enters_sec(self):
        for kwargs in [dict(flip_fail=True),dict(bad_scanout=True),dict(reject_key_input=True)]:
            with self.subTest(**kwargs): self.assertFalse(self.run_mode(2,**kwargs)['entry'])


if __name__=='__main__': unittest.main()
