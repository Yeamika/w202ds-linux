"""Exercise the actual minimal-LK bytes, including native watchdog code."""
import os
from pathlib import Path
import runpy
import struct
import unittest

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import UC_PROT_READ, UC_PROT_WRITE, UC_PROT_EXEC
from unicorn.arm64_const import *

ROOT = Path(__file__).resolve().parents[1]
API = runpy.run_path(str(ROOT/'debug/image.py'))
BASE, ARG, STACK, END = 0x86000000, 0x8a001000, 0xb5e00000, 0xb6001000
PTE, ORIGINAL, EXEC = 0xb5f01180, 0x0060000086000711, 0x0000000086000791
MONITOR, CONTROL = 0x644223ac, 0x64420048


def registered_commands(image):
    """Discover handlers by executing LK's native Fastboot registrations."""
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    u.mem_map(0xb5000000, 0x1000000)
    u.mem_write(0xb5000000, image[512:0x113a60])
    commands = {}
    def register(uc, pc, size, data):
        if pc == 0xb500f06c:
            name = bytes(uc.mem_read(uc.reg_read(UC_ARM64_REG_X0), 64))
            name = name.split(b'\0', 1)[0].decode()
            commands[name] = uc.reg_read(UC_ARM64_REG_X1)
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
    u.hook_add(UC_HOOK_CODE, register)
    u.emu_start(0xb5012c90, 0xb5012d1c, count=1000)
    if u.reg_read(UC_ARM64_REG_PC) != 0xb5012d1c:
        raise AssertionError('native Fastboot registrations did not finish')
    return commands


class DebuggerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.builds = {name:Path(os.environ['LK_'+name.upper()+'_BUILD'])
                      for name in ('debug','release','watchdog','usb','screen')}
        cls.images = {name:(path/'lk-debuger.img').read_bytes() for name,path in cls.builds.items()}
        cls.smoke = (cls.builds['debug']/'ramboot-smoke.bin').read_bytes()
        cls.hang = (cls.builds['debug']/'ramboot-hang.bin').read_bytes()

    def run_image(self, name='debug', *, command=' jump-rmk :probe',
                  mode=None, handler=None, corrupt=False, size=None, cpu=1,
                  el=8, bad_pte=False, fail_reason=False, hang=False, raw=False,
                  native_entry=None, download_okay=True, lcd_enabled=True):
        image = self.images[name]
        u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.mem_map(0xb5000000,0x1000000)
        u.mem_write(0xb5000000,image[512:0x113a60])
        u.mem_map(BASE,0x200000,UC_PROT_READ|UC_PROT_WRITE)
        u.mem_map(ARG,0x1000)
        u.mem_map(END,0x1000)
        payload = bytearray(self.hang if hang else self.smoke)
        if corrupt: payload[8:12]=b'BAD!'
        u.mem_write(BASE,bytes(payload))
        u.mem_write(ARG,command.encode()+b'\0')
        u.mem_write(PTE,struct.pack('<Q',ORIGINAL+0x200000 if bad_pte else ORIGINAL))
        u.mem_write(0xb50eed8c,struct.pack('<I',cpu))
        u.mem_write(0xb56bae24,bytes([lcd_enabled]))
        u.mem_write(0xb56bb054,struct.pack('<HH',150,120))
        initial={UC_ARM64_REG_X18:0x1818, UC_ARM64_REG_X29:0x2929,
                 **{UC_ARM64_REG_X19+i:0x1900+i for i in range(10)}}
        if mode is not None: initial[UC_ARM64_REG_X19]=mode
        for reg,value in initial.items(): u.reg_write(reg,value)
        u.reg_write(UC_ARM64_REG_SP,STACK)
        u.reg_write(UC_ARM64_REG_LR,END)
        u.reg_write(UC_ARM64_REG_X0,ARG if mode is None else handler)
        u.reg_write(UC_ARM64_REG_X1,BASE)
        u.reg_write(UC_ARM64_REG_X2,len(payload) if size is None else size)
        state={'pmic':{MONITOR:0x1200},'starts':[],'stops':0,'writes':[],
               'responses':[],'transitions':[],'payload':False,'native':None,
               'lcd':[], 'native_downloads':0}

        def string(address):
            return bytes(u.mem_read(address,128)).split(b'\0',1)[0].decode()
        def ret(value=0):
            u.reg_write(UC_ARM64_REG_X0,value&0xffffffffffffffff)
            u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
        def hardware(uc,pc,n,_):
            x0,x1,x2=(uc.reg_read(r) for r in (UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2))
            ins=int.from_bytes(uc.mem_read(pc,4),'little')
            if ins&~31==0xd5384240:
                uc.reg_write(UC_ARM64_REG_X0+(ins&31),el)
                uc.reg_write(UC_ARM64_REG_PC,pc+4)
            elif pc==0xb506ce54: ret(state['pmic'].get(x0,0))
            elif pc==0xb506cf70:
                state['writes'].append((x0,x1))
                if not (fail_reason and x0==MONITOR and x1&255==0x30):
                    state['pmic'][x0]=x1&0xffff
                ret()
            elif pc==0xb506d0dc:
                state['pmic'][x0]=((state['pmic'].get(x0,0)&~x2)|x1)&0xffff
                ret()
            elif pc==0xb5093220: state['starts'].append(x0)
            elif pc==0xb50932a0: state['stops']+=1
            elif pc==0xb500e11c:
                state['responses'].append(string(x0)); ret()
            elif pc==0xb5087d98:
                state['lcd'].append(string(x0))
                # The LCD ABI may clobber every volatile argument register.
                for reg in range(UC_ARM64_REG_X1, UC_ARM64_REG_X17+1):
                    uc.reg_write(reg,0xc100+reg)
                ret()
            elif pc==0xb503f5d0:
                self.fail('screen/debug hooks must not write eMMC')
            elif native_entry=='download' and pc==__import__('json').loads(
                    (self.builds[name]/'manifest.json').read_text())['symbols'].get('lk_debug_download_raw',0xb500e554):
                state['native_downloads']+=1
                self.assertEqual((x0,x1,x2),(ARG,BASE,len(payload)))
                u.mem_write(0xb5122000,b'OKAY\0' if download_okay else b'FAIL\0')
                u.mem_write(0xb5122040,struct.pack('<I',2))
                if not API['validate'](image)['fastboot_flash']:
                    frame=u.reg_read(UC_ARM64_REG_SP)
                    fp,lr=struct.unpack('<QQ',bytes(u.mem_read(frame,16)))
                    u.reg_write(UC_ARM64_REG_X29,fp)
                    u.reg_write(UC_ARM64_REG_LR,lr)
                    u.reg_write(UC_ARM64_REG_SP,frame+256)
                ret(0x1234)
            elif native_entry=='fastboot' and pc==0xb5012a84:
                self.assertEqual((x0,x1,x2),(ARG,BASE,len(payload)))
                self.assertEqual(u.reg_read(UC_ARM64_REG_SP),STACK-272)
                state['native']='fastboot'; u.emu_stop()
            elif pc in (0xb5004dc0,0xb5035e94,0xb5035c78): ret()
            elif pc==0xb500e2a4:
                pte=int.from_bytes(uc.mem_read(PTE,8),'little')
                state['transitions'].append(pte)
                uc.mem_protect(BASE,0x200000,UC_PROT_READ|(UC_PROT_EXEC if pte==EXEC else UC_PROT_WRITE))
                uc.reg_write(UC_ARM64_REG_PC,pc+4)
            elif pc==0xb500e2ac: uc.reg_write(UC_ARM64_REG_PC,pc+4)
            elif pc==BASE+32:
                state['payload']=True
                state['operation']=string(x0)
                state['armed_at_entry']=state['pmic'].get(CONTROL,0)&0xa==0xa
                state['reason_at_entry']=state['pmic'][MONITOR]&255
                if hang: uc.emu_stop()
            elif pc==0xb5011214:
                state['native']='oem'; uc.emu_stop()
            elif mode is not None and pc in (handler,0xb500ddf0):
                state['native']=pc; uc.emu_stop()
        u.hook_add(UC_HOOK_CODE,hardware)
        commands = registered_commands(image)
        start=0xb5006670 if mode is not None else (commands['jump-rmk'] if raw else commands['oem'])
        if native_entry is not None:
            start={'download':0xb500e550,'fastboot':0xb5012a80}[native_entry]
        u.emu_start(start,END,count=1000000)
        if state['native'] is None and not hang:
            self.assertEqual(u.reg_read(UC_ARM64_REG_PC),END)
            self.assertEqual(u.reg_read(UC_ARM64_REG_SP),STACK)
            for reg,value in initial.items(): self.assertEqual(u.reg_read(reg),value)
        state['vm']=u
        return state

    def test_native_registration_reaches_sdk_patch_and_keeps_raw_entry(self):
        for name, image in self.images.items():
            with self.subTest(name=name):
                legacy = (self.builds[name]/'ramlk/lk-jump-rmk.img').read_bytes()
                commands = registered_commands(image)
                self.assertEqual(commands, registered_commands(legacy))
                self.assertEqual(commands['jump-rmk'], 0xb500e1d0)
                entry = commands['oem'] - API['LK'] + 512
                self.assertNotEqual(image[entry:entry+4], legacy[entry:entry+4])
                # The unrelated native serial-number handler must stay intact.
                self.assertEqual(image[0xf728:0xf72c], legacy[0xf728:0xf72c])

    def test_sdk_probe_returns_and_restores_watchdog_monitor_and_mapping(self):
        s=self.run_image()
        self.assertEqual(s['responses'],['OKAY'])
        self.assertEqual(s['operation'],':probe')
        self.assertTrue(s['armed_at_entry'])
        self.assertEqual(s['reason_at_entry'],0x30)
        self.assertEqual(s['starts'],[30000])
        self.assertEqual(s['stops'],2)
        self.assertEqual(s['pmic'][MONITOR],0x1200)
        # Native stop_watchdog clears CNT_EN (bit 1); reset enable may remain.
        self.assertFalse(s['pmic'][CONTROL]&2)
        self.assertEqual(s['transitions'],[EXEC,ORIGINAL])

    def test_sdk_default_and_colon_form(self):
        for command in (' jump-rmk',' jump-rmk:probe','jump-rmk :init'):
            with self.subTest(command=command):
                s=self.run_image(command=command)
                self.assertEqual(s['responses'],['OKAY'])
                self.assertIn(s['operation'],(':probe',':init'))

    def test_raw_entry_remains_compatible(self):
        s=self.run_image(raw=True,command=':probe')
        self.assertEqual(s['responses'],['OKAY'])
        self.assertTrue(s['armed_at_entry'])

    def test_hung_payload_has_live_native_reset_watchdog_and_usb_boot_reason(self):
        s=self.run_image(hang=True)
        self.assertTrue(s['armed_at_entry'])
        self.assertEqual(s['starts'],[30000])
        self.assertEqual(s['reason_at_entry'],0x30)
        self.assertFalse(s['responses'])
        s=self.run_image(mode=5,handler=0xb500ddf0)
        self.assertEqual(s['native'],0xb500ddf0)

    def test_invalid_payload_or_unsupported_machine_never_runs_or_arms(self):
        for options in (dict(corrupt=True),dict(size=31),dict(size=39),dict(cpu=2),
                        dict(el=4),dict(bad_pte=True)):
            with self.subTest(**options):
                s=self.run_image(**options)
                self.assertFalse(s['payload'])
                self.assertFalse(s['starts'])
                self.assertFalse(s['transitions'])
                self.assertEqual(s['responses'],['FAIL'])

    def test_rejected_reset_reason_aborts_before_payload(self):
        s=self.run_image(fail_reason=True)
        self.assertEqual(s['responses'],['FAIL'])
        self.assertFalse(s['payload'])
        self.assertFalse(s['starts'])
        self.assertEqual(s['pmic'][MONITOR],0x1200)

    def test_normal_external_reset_use_native_fastboot(self):
        for mode in (2,16):
            with self.subTest(mode=mode):
                s=self.run_image(mode=mode,handler=0xb500da88)
                self.assertEqual(s['native'],0xb500ddf0)
                self.assertFalse(s['starts'])

    def test_other_modes_and_unexpected_normal_handler_stay_native(self):
        for mode,handler in ((4,0xb500db88),(5,0xb500ddf0),(7,0xb500ded0),
                             (3,0xb500dc80),(2,0xb500db88)):
            with self.subTest(mode=mode,handler=handler):
                self.assertEqual(self.run_image(mode=mode,handler=handler)['native'],handler)

    def test_native_oem_commands_preserve_arguments_and_original_prologue(self):
        for command in (' unlock',' get_socid',' jump-rm',' jump-rmk-bad'):
            with self.subTest(command=command):
                s=self.run_image(command=command)
                u=s['vm']
                self.assertEqual(s['native'],'oem')
                self.assertEqual(u.reg_read(UC_ARM64_REG_SP),STACK-0x3a0)
                self.assertEqual(u.reg_read(UC_ARM64_REG_X0),ARG)
                self.assertEqual(u.reg_read(UC_ARM64_REG_X1),BASE)
                self.assertEqual(u.reg_read(UC_ARM64_REG_X2),len(self.smoke))
                self.assertFalse(s['writes'])

    def test_debug_off_keeps_sdk_ramboot_without_watchdog_or_forced_usb(self):
        s=self.run_image('release')
        self.assertEqual(s['responses'],['OKAY'])
        self.assertFalse(s['starts'])
        self.assertFalse(s['writes'])
        self.assertFalse(s['lcd'])
        self.assertEqual(self.run_image('release',mode=2,handler=0xb500da88)['native'],0xb500da88)

    def test_native_console_reports_running_return_and_rejection(self):
        s=self.run_image()
        actions=[line for line in s['lcd'] if line.strip() and line!='PJ4EDK2 LK DEBUG']
        self.assertEqual(actions,['RAMBoot executing; watchdog armed',
                                  'RAMBoot returned OK; Fastboot ready'])
        s=self.run_image(corrupt=True)
        self.assertIn('RAMBoot rejected or returned error; check host output',s['lcd'])
        self.assertNotIn('RAMBoot executing; watchdog armed',s['lcd'])
        self.assertFalse(self.run_image(lcd_enabled=False)['lcd'])

    def test_native_fastboot_console_keeps_original_entry_abi(self):
        s=self.run_image(native_entry='fastboot')
        self.assertEqual(s['native'],'fastboot')
        self.assertIn('Fastboot ready; waiting for host',s['lcd'])

    def test_native_download_console_keeps_usb_handler_and_result(self):
        for okay in (True,False):
            with self.subTest(okay=okay):
                s=self.run_image(native_entry='download',download_okay=okay)
                self.assertEqual(s['native_downloads'],1)
                self.assertEqual(s['vm'].reg_read(UC_ARM64_REG_X0),0x1234)
                self.assertIn('RAM download in progress',s['lcd'])
                expected='RAM download OK; ready to execute' if okay else 'RAM download failed; check host output'
                self.assertIn(expected,s['lcd'])

    def test_individual_build_flags(self):
        for name,watchdog,usb in (('watchdog',True,False),('usb',False,True),('screen',False,False)):
            with self.subTest(name=name):
                s=self.run_image(name)
                self.assertEqual(bool(s['starts']),watchdog)
                expected=0xb500ddf0 if usb else 0xb500da88
                self.assertEqual(self.run_image(name,mode=2,handler=0xb500da88)['native'],expected)
                self.assertEqual(bool(s['lcd']),name=='screen')
        self.assertIn('RAMBoot executing; watchdog disabled',self.run_image('screen')['lcd'])

    def test_checked_family_accepts_all_variants_and_rejects_corruption(self):
        pack=runpy.run_path(str(ROOT/'ramboot/package-resident.py'))
        for data in self.images.values():
            self.assertEqual(API['validate'](data)['kind'],'minimal-ramlk')
            self.assertEqual(pack['validate_sources'](data)['kind'],'minimal-ramlk')
        for offset in (0x310,0x1400,0x114000,0x228000,0x6870,0x12c80,0xe750,
                       API['HEADER_OFFSET']+API['CONFIG'].size):
            with self.subTest(offset=hex(offset)):
                bad=bytearray(self.images['debug']);bad[offset]^=1
                with self.assertRaises(ValueError): pack['validate_sources'](bad)


if __name__=='__main__': unittest.main()
