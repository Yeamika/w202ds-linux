"""Execute the actual ARM64 gateway with modeled privileged registers.

Checks ABI, address-space switches and RAM-only display suppression. This does
not claim to simulate hardware cache coherency, USB enumeration or eMMC timing.
"""
import os
from pathlib import Path
import re
import struct
import subprocess
import unittest
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_READ
from unicorn import arm64_const as reg

class GatewayTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.elf=Path(os.environ['LK_FASTBOOT_ELF'])
        cls.image=cls.elf.with_suffix('.bin').read_bytes()
        dis=subprocess.check_output([os.environ.get('AARCH64_OBJDUMP','aarch64-linux-gnu-objdump'),
                                     '-d',str(cls.elf)],text=True)
        cls.symbols={m[2]:int(m[1],16) for m in re.finditer(r'^([0-9a-f]+) <([^>]+)>:',dis,re.M)}
        cls.ins={int(m[1],16):(m[2],m[3].split('//')[0].strip())
                 for m in re.finditer(r'^\s*([0-9a-f]+):\s+[0-9a-f]+\s+(\S+)\s*(.*)$',dis,re.M)}

    def execute(self, operation=2, result=1, el=8):
        uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        for address,size in [(0x86000000,0x10000),(0x86400000,0x40000),
                             (0xb5000000,0x1000000),(0xc8000000,0x10000),(0xcc001000,0x1000)]:
            uc.mem_map(address,size)
        uc.mem_write(0x86000000,self.image)
        text={0xb5035e94:b'ABCDEFGH',0xb5035c78:b'12345678',0xb5087d98:b'LCDTEXT!'}
        for address,body in text.items():uc.mem_write(address,body)
        status_original=b'STATUS-ORIGINAL!'
        uc.mem_write(0xb500e11c,status_original)
        def R(name):return getattr(reg,'UC_ARM64_REG_'+name.upper())
        original={name:0x111100+i for i,name in enumerate(
            ['sctlr_el2','ttbr0_el2','tcr_el2','mair_el2','vbar_el2','hcr_el2','cptr_el2',
             'tpidr_el2','tpidr_el1','tpidr_el0','sp_el0','sp_el1','vbar_el1','sctlr_el1',
             'cpacr_el1','fpcr','fpsr','tpidrro_el0','mdcr_el2'])}
        original['daif']=0x80
        original['csselr_el1']=2
        system=dict(original, currentel=el, clidr_el1=(1<<24)|4, ccsidr_el1=2)
        layout={23:'sctlr_el2',24:'ttbr0_el2',25:'tcr_el2',26:'mair_el2',27:'vbar_el2',
                28:'hcr_el2',29:'cptr_el2',30:'tpidr_el2',31:'tpidr_el1',32:'tpidr_el0',
                33:'sp_el0',34:'sp_el1',35:'vbar_el1',36:'sctlr_el1',37:'cpacr_el1',
                38:'fpcr',39:'fpsr',55:'tpidrro_el0',56:'mdcr_el2'}
        for index in layout:uc.mem_write(0x86400000+index*8,struct.pack('<Q',0x222200+index))
        uc.mem_write(0x86400008,struct.pack('<Q',0x222218))
        registers={f'x{i}':0x770000+i for i in range(18,31)}
        registers.update({f'd{i}':0x880000+i for i in range(8,16)})
        registers.update(sp=0xc800f000,x30=0xcc001000)
        for name,value in registers.items():uc.reg_write(R(name),value)
        uc.reg_write(R('x0'),operation)
        calls=[]
        def step(u,address,size,unused):
            if address==0xcc001000:u.emu_stop();return
            if address==self.symbols['lk_fb_service']:
                calls.append(u.reg_read(R('x0')))
                self.assertEqual(calls,[operation])
                self.assertEqual(u.reg_read(R('sp')),0x86440000)
                self.assertEqual(u.reg_read(R('x18')),0x222218)
                for index,name in layout.items():
                    if name not in ('fpcr','fpsr'):
                        self.assertEqual(system[name],0x222200+index,(name,'LK context'))
                self.assertEqual(system['daif']&0x3c0,0x3c0)
                self.assertEqual(bytes(u.mem_read(0xb500e11c,16)),struct.pack('<IIQ',0x18000050,0xd61f0200,self.symbols['lk_fb_capture_status']))
                for a in text:self.assertEqual(bytes(u.mem_read(a,8)),bytes.fromhex('00008052c0035fd6'))
                lr=u.reg_read(R('x30'))
                for name in registers:
                    if name!='sp':u.reg_write(R(name),0xaabb)
                u.reg_write(R('x0'),result&0xffffffffffffffff)
                u.reg_write(reg.UC_ARM64_REG_PC,lr);return
            op,args=self.ins.get(address,('',''))
            if op in ('mrs','msr'):
                a,b=[s.strip().lower() for s in args.split(',')]
                if op=='mrs':u.reg_write(R(a),system.get(b,0))
                elif a=='daifset':system['daif']|=0x3c0
                else:system[a]=0 if b=='xzr' else u.reg_read(R(b))
                u.reg_write(reg.UC_ARM64_REG_PC,address+4)
            elif op in ('ic','dc','dsb','isb','tlbi'):
                u.reg_write(reg.UC_ARM64_REG_PC,address+4)
        def aligned(u,kind,address,size,value,unused):
            if size==8:self.assertEqual(address%8,0,"MMU-off native text access must be aligned")
        uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE,aligned)
        def write(u,kind,address,size,value,unused):
            self.assertTrue(0x86408000<=address<0x86408400 or
                0xb500e11c<=address<0xb500e12c or any(a<=address<a+8 for a in text),f'unexpected write {address:x}')
        uc.hook_add(UC_HOOK_CODE,step)
        uc.hook_add(UC_HOOK_MEM_WRITE,write)
        uc.emu_start(self.symbols['lk_fb_gateway'],0xcc001000,count=30000)
        self.assertEqual(uc.reg_read(reg.UC_ARM64_REG_PC),0xcc001000)
        for name,value in registers.items():self.assertEqual(uc.reg_read(R(name)),value,name)
        for name,value in original.items():self.assertEqual(system[name],value,name)
        self.assertEqual(bytes(uc.mem_read(0xb500e11c,16)),status_original)
        for address,body in text.items():self.assertEqual(bytes(uc.mem_read(address,8)),body)
        self.assertEqual(uc.reg_read(R('x0')),(result if el==8 else -1)&0xffffffffffffffff)
        self.assertEqual(calls,[operation] if el==8 else [])

    def test_status_thunk_uses_only_four_byte_aligned_native_accesses(self):
        u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
        u.mem_map(0xb500e000,0x1000)
        u.mem_map(0x86000000,0x1000)
        u.mem_map(0xcc001000,0x1000)
        # Native status is four-byte aligned; its literal must use LDR W.
        u.mem_write(0xb500e11c,struct.pack('<IIQ',0x18000050,0xd61f0200,0x86000000))
        u.mem_write(0x86000000,bytes.fromhex('c0035fd6'))
        u.reg_write(reg.UC_ARM64_REG_X0,0x1234)
        u.reg_write(reg.UC_ARM64_REG_X1,0x5678)
        u.reg_write(reg.UC_ARM64_REG_X30,0xcc001000)
        def aligned(uc,kind,address,size,value,unused):
            self.assertEqual(address%size,0)
        u.hook_add(UC_HOOK_MEM_READ,aligned)
        u.emu_start(0xb500e11c,0xcc001000,count=10)
        self.assertEqual(u.reg_read(reg.UC_ARM64_REG_PC),0xcc001000)
        self.assertEqual(u.reg_read(reg.UC_ARM64_REG_X0),0x1234)
        self.assertEqual(u.reg_read(reg.UC_ARM64_REG_X1),0x5678)

    def test_native_probe_preserves_service_and_restores_execute_permissions(self):
        reader=bytearray((Path(os.environ['LK_MEMORY_BUILD'])/'ramboot-memory.bin').read_bytes())
        struct.pack_into('<QQ',reader,16,0x82ffe000,16)
        for version in (1,2):
            with self.subTest(version=version):
                struct.pack_into('<I',reader,12,version)
                u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
                for a,n in ((0x86000000,0x10000),(0x86200000,0x200000),
                            (0x86400000,0x40000),(0x88000000,0x10000),
                            (0x82ffe000,0x2000),(0xb5000000,0x1000000),
                            (0xcc001000,0x1000)):
                    u.mem_map(a,n)
                u.mem_write(0x86000000,self.image)
                u.mem_write(0x88000000,bytes(reader))
                old=0x0060000086200701
                u.mem_write(0xb5f01188,struct.pack('<Q',old))
                u.mem_write(0x82ffe000,bytes(range(16)))
                u.reg_write(reg.UC_ARM64_REG_SP,0x86440000)
                u.reg_write(reg.UC_ARM64_REG_X30,0xcc001000)
                u.reg_write(reg.UC_ARM64_REG_X0,0x88000000)
                u.reg_write(reg.UC_ARM64_REG_X1,len(reader))
                packets=[]
                def step(uc,pc,size,unused):
                    if pc==0xb5004dc0:
                        self.assertEqual(uc.reg_read(reg.UC_ARM64_REG_X0),0x86200000)
                        uc.reg_write(reg.UC_ARM64_REG_PC,uc.reg_read(reg.UC_ARM64_REG_X30))
                    elif pc==0xb500e11c:
                        a=uc.reg_read(reg.UC_ARM64_REG_X1)
                        packets.append(bytes(uc.mem_read(a,60)).split(b'\0')[0])
                        uc.reg_write(reg.UC_ARM64_REG_PC,uc.reg_read(reg.UC_ARM64_REG_X30))
                    elif self.ins.get(pc,('',))[0] in ('dc','dsb','tlbi','isb'):
                        uc.reg_write(reg.UC_ARM64_REG_PC,pc+4)
                u.hook_add(UC_HOOK_CODE,step)
                u.emu_start(self.symbols['probe'],0xcc001000,count=100000)
                self.assertEqual(u.reg_read(reg.UC_ARM64_REG_PC),0xcc001000)
                self.assertEqual(u.reg_read(reg.UC_ARM64_REG_SP),0x86440000)
                self.assertEqual(bytes(u.mem_read(0x86000000,len(self.image))),self.image)
                self.assertEqual(struct.unpack('<Q',bytes(u.mem_read(0xb5f01188,8)))[0],old)
                self.assertEqual(u.reg_read(reg.UC_ARM64_REG_X0)==0,version==2)
                self.assertEqual(len(packets),3 if version==2 else 0)

    def test_repeated_service_calls_restore_efi_and_print(self):
        for operation,result in [(1,0),(2,1),(2,0),(2,-1),(3,0)]:
            with self.subTest(operation=operation,result=result):self.execute(operation,result)

    def test_wrong_exception_level_never_enters_lk(self):
        self.execute(el=4)

if __name__=='__main__':unittest.main()
