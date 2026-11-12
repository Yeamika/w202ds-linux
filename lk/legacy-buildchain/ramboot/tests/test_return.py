#!/usr/bin/env python3
"""Execute the return assembly; model privileged registers and native calls.

This checks the ABI and failure control flow, not real GIC/cache/MMU hardware.
"""
import os
from pathlib import Path
import re
import struct
import subprocess
import unittest
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_PROT_READ, UC_PROT_WRITE
from unicorn import arm64_const as reg

BASE=0x86000000
CTX=0x86400000
END=0xb6000000
EFI=0x82801000


class ReturnTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        elf=Path(os.environ['LK_RETURN_ELF'])
        cls.image=elf.with_suffix('.bin').read_bytes()
        tool=os.environ['AARCH64_OBJDUMP']
        text=subprocess.check_output([tool,'-d',str(elf)],text=True)
        cls.symbols={m[2]:int(m[1],16) for m in re.finditer(r'^([0-9a-f]+) <([^>]+)>:',text,re.M)}
        cls.instructions={int(m[1],16):(m[2],m[3].split('//')[0].strip())
                          for m in re.finditer(r'^\s*([0-9a-f]+):\s+[0-9a-f]+\s+(\S+)\s*(.*)$',text,re.M)}

    def run_return(self,action=0,save_fail=False,restore_fail=False):
        u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        for address,size in [(BASE,0x10000),(CTX,0x10000),(0x82fff000,0x1000),
                             (EFI,0x1000),(0xb5000000,0x1000000),(END,0x1000)]:
            u.mem_map(address,size)
        u.mem_write(BASE,self.image)
        initial={}
        for i in range(18,31):
            r=getattr(reg,f'UC_ARM64_REG_X{i}')
            initial[r]=0x11110000+i
        initial[reg.UC_ARM64_REG_X30]=END
        initial[reg.UC_ARM64_REG_SP]=0xb5e00000
        for i in range(8,16):initial[getattr(reg,f'UC_ARM64_REG_D{i}')]=0x12340000+i
        for r,value in initial.items():u.reg_write(r,value)
        u.reg_write(reg.UC_ARM64_REG_X0,EFI)
        original={args.split(',')[1].strip():0x100+i
                  for i,(address,(op,args)) in enumerate(self.instructions.items())
                  if op=='mrs' and self.symbols['handoff_enter_sec_returnable']<=address<self.symbols['save_failed']}
        system=dict(original)
        events=[]

        def trace_stage():
            return struct.unpack('<Q', bytes(u.mem_read(0x82fff008, 8)))[0]

        def cpu(uc,address,size,data):
            if address==END:
                events.append('returned');uc.emu_stop();return
            if address==self.symbols['restore_failed']:
                events.append('watchdog');uc.emu_stop();return
            if address in (self.symbols['lk_return_save'],self.symbols['lk_return_restore']):
                save=address==self.symbols['lk_return_save']
                if save:self.assertEqual(trace_stage(),0x11)
                events.append('save' if save else 'restore')
                uc.reg_write(reg.UC_ARM64_REG_X0,0xffffffff if (save_fail if save else restore_fail) else 0)
                uc.reg_write(reg.UC_ARM64_REG_PC,uc.reg_read(reg.UC_ARM64_REG_X30));return
            if address==0xb500b904:
                self.assertEqual(trace_stage(),0x12)
                events.append('cleanup')
                uc.reg_write(reg.UC_ARM64_REG_PC,uc.reg_read(reg.UC_ARM64_REG_X30));return
            if address==EFI:
                self.assertEqual(trace_stage(),0x22)
                events.append('efi')
                for r in initial:uc.reg_write(r,0)
                uc.reg_write(reg.UC_ARM64_REG_SP,0xb5d00000)
                for key in system:system[key]=0
                uc.reg_write(reg.UC_ARM64_REG_X0,action)
                uc.reg_write(reg.UC_ARM64_REG_PC,self.symbols['lk_return_resume']);return
            op,args=self.instructions.get(address,('', ''))
            if op in ('mrs','msr'):
                a,b=[v.strip() for v in args.split(',')]
                if op=='mrs':uc.reg_write(getattr(reg,'UC_ARM64_REG_'+a.upper()),system.get(b,0))
                elif a=='daifset':system['daif']=system['daif']|0x3c0
                else:system[a]=0 if b=='xzr' else uc.reg_read(getattr(reg,'UC_ARM64_REG_'+b.upper()))
                uc.reg_write(reg.UC_ARM64_REG_PC,address+4)
            elif op in ('dc','ic','tlbi'):
                uc.reg_write(reg.UC_ARM64_REG_PC,address+4)
        u.hook_add(UC_HOOK_CODE,cpu)
        u.emu_start(self.symbols['handoff_enter_sec_returnable'],END+4,count=3000)
        if restore_fail:
            self.assertEqual(events,['save','cleanup','efi','restore','watchdog']);return
        for r,value in initial.items():self.assertEqual(u.reg_read(r),value,f'reg {r}')
        self.assertEqual(u.reg_read(reg.UC_ARM64_REG_X0),0xffffffff if save_fail else action)
        self.assertEqual(events,['save','returned'] if save_fail else ['save','cleanup','efi','restore','returned'])
        if save_fail:self.assertEqual(trace_stage(),0x13)
        if not save_fail:
            for key,value in original.items():self.assertEqual(system[key],value,key)

    def test_probe_roundtrip(self):self.run_return()
    def test_android_action_survives_restore(self):self.run_return(action=1)
    def test_save_rejection_preserves_caller(self):self.run_return(save_fail=True)
    def test_restore_failure_never_returns(self):self.run_return(restore_fail=True)
    def test_loader_fits_before_image(self):self.assertLessEqual(len(self.image),0x10000)

    def test_native_continue_preserves_usb_completion(self):
        # Execute the actual pinned native continue body as well as our thunk.
        # A RAM command needs its OKAY; a cold resident has no pending command.
        stock=Path(os.environ['LK_RETURN_RESIDENT_IMAGE']).read_bytes()[512:0x113a60]
        self.assertEqual(stock[0xe2bc:0xe2c0],bytes.fromhex('a0080090'))
        for active,cold in ((0,False),(1,False),(0,True)):
            u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
            u.mem_map(BASE,0x10000);u.mem_write(BASE,self.image)
            u.mem_map(0xb5000000,0x1000000);u.mem_write(0xb5000000,stock)
            u.mem_write(0xb5122040,struct.pack('<I',active))
            u.mem_write(0xb5f01180,struct.pack('<Q',0x0000000086000791))
            u.reg_write(reg.UC_ARM64_REG_SP,0xb5e00000)
            u.reg_write(reg.UC_ARM64_REG_X19,0x12345678)
            u.reg_write(reg.UC_ARM64_REG_X0,0xb500da8c if cold else 0xb500e2c8)
            events=[]
            def call(uc,address,size,data):
                if address==0xb5020ea4:events.append('boot');uc.emu_stop()
                elif address==0xb5093220:
                    # A long menu or an unclean Android filesystem must not
                    # inherit the nearly expired RAMBoot watchdog deadline.
                    self.assertEqual(uc.reg_read(reg.UC_ARM64_REG_X0),180000)
                    self.assertEqual(uc.reg_read(reg.UC_ARM64_REG_SP)%16,0)
                    events.append('watchdog')
                    # Native calls may destroy all caller-saved registers.
                    # The selected boot target must survive that call.
                    resume=uc.reg_read(reg.UC_ARM64_REG_X30)
                    for i in range(18):uc.reg_write(getattr(reg,f'UC_ARM64_REG_X{i}'),0xdead0000+i)
                    uc.reg_write(reg.UC_ARM64_REG_PC,resume)
                elif address==0xb500e2a4:
                    # Permission change must happen while executing native LK.
                    self.assertEqual(struct.unpack('<Q',bytes(uc.mem_read(0xb5f01180,8)))[0],0x0060000086000711)
                    uc.mem_protect(BASE,0x10000,UC_PROT_READ|UC_PROT_WRITE)
                    uc.reg_write(reg.UC_ARM64_REG_PC,address+4)
                elif 0xb500e2a8<=address<=0xb500e2b4:
                    uc.reg_write(reg.UC_ARM64_REG_PC,address+4)
                elif address in (0xb500e11c,0xb5085770):
                    events.append('okay' if address==0xb500e11c else 'usb-stop')
                    uc.reg_write(reg.UC_ARM64_REG_PC,uc.reg_read(reg.UC_ARM64_REG_X30))
                elif address in (0xb5079b14,0xb5086ca4,0xb5086c40):
                    uc.reg_write(reg.UC_ARM64_REG_PC,uc.reg_read(reg.UC_ARM64_REG_X30))
            u.hook_add(UC_HOOK_CODE,call)
            u.emu_start(self.symbols['lk_return_android'],0,count=100)
            self.assertEqual(events,['watchdog']+(['boot'] if cold else (['okay'] if active else [])+['usb-stop','boot']))
            self.assertEqual(u.reg_read(reg.UC_ARM64_REG_SP),0xb5e00000)
            self.assertEqual(u.reg_read(reg.UC_ARM64_REG_X19),0x12345678)
            self.assertEqual(u.reg_read(reg.UC_ARM64_REG_X30),0xb5012a80)


if __name__=='__main__':unittest.main()
