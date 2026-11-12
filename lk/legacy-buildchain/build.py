#!/usr/bin/env python3
"""Build a minimal RAMBoot LK with optional watchdog, recovery and LCD status."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
IMAGE = runpy.run_path(str(ROOT/'debug/image.py'))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True, help='fresh directory outside the repository')
    p.add_argument('--base', type=Path, default=ROOT.parent/'firmware/uboot-mod.bin')
    p.add_argument('--cross-compile', default='aarch64-linux-gnu-')
    p.add_argument('--debug', action=argparse.BooleanOptionalAction, default=True,
                   help='default for watchdog, automatic USB recovery and LCD status')
    p.add_argument('--watchdog', action=argparse.BooleanOptionalAction, default=None)
    p.add_argument('--usb-recovery', action=argparse.BooleanOptionalAction, default=None)
    p.add_argument('--screen', action=argparse.BooleanOptionalAction, default=None)
    p.add_argument('--fastboot-flash', action=argparse.BooleanOptionalAction, default=True,
                   help='verified whole-image flash to an explicit GPT _a/_b partition')
    p.add_argument('--watchdog-ms', type=int, default=30000, help='payload timeout, 1000..180000 ms')
    a = p.parse_args()
    out = a.output.resolve()
    if out == REPO or REPO in out.parents or out.exists():
        p.error('choose a fresh output directory outside the repository')
    if not 1000 <= a.watchdog_ms <= 180000:
        p.error('--watchdog-ms must be between 1000 and 180000')
    watchdog = a.debug if a.watchdog is None else a.watchdog
    usb = a.debug if a.usb_recovery is None else a.usb_recovery
    screen = a.debug if a.screen is None else a.screen
    out.mkdir(parents=True)
    commands = []
    def run(*args):
        argv = [str(x) for x in args]
        commands.append(argv)
        subprocess.run(argv, check=True)
    run(sys.executable, ROOT/'ramlk/build.py', '--output', out/'ramlk',
        '--base', a.base, '--cross-compile', a.cross_compile)
    ext = out/'debug'
    ext.mkdir()
    run(a.cross_compile+'gcc', '-march=armv8-a',
        f'-DW202_DEBUG_WATCHDOG={int(watchdog)}', f'-DW202_DEBUG_USB_RECOVERY={int(usb)}',
        f'-DW202_DEBUG_SCREEN={int(screen)}',
        f'-DW202_FASTBOOT_FLASH={int(a.fastboot_flash)}',
        f'-DW202_DEBUG_WATCHDOG_MS={a.watchdog_ms}', '-c', ROOT/'debug/hook.S', '-o', ext/'hook.o')
    defs = {'lk_jump':0xb500e1d0, 'lk_oem_continue':0xb5011214,
            'lk_fastboot_mode':0xb500ddf0, 'lk_icache_sync':0xb5004dc0,
            'permissions':0xb500e2a0, 'ptable_page':0xb5f01000,
            'okay_page':0xb50bc000, 'lk_status':0xb500e11c,
            'lk_stop_watchdog':0xb50932a0, 'lk_start_watchdog':0xb5093220,
            'lk_adi_read':0xb506ce54, 'lk_adi_write':0xb506cf70,
            'lk_fastboot_continue':0xb5012a84, 'lk_download_continue':0xb500e554,
            'lk_lcd_enabled':0xb56bae24, 'lk_lcd_position':0xb508796c,
            'lk_lcd_puts':0xb5087d98}
    run(a.cross_compile+'gcc', '-march=armv8-a', '-c', ROOT/'debug/screen.S', '-o', ext/'screen.o')
    if a.fastboot_flash:
        for name in ('flash','download'):
            run(a.cross_compile+'gcc', '-Os', '-Wall', '-Wextra', '-Werror', '-ffreestanding',
                '-fno-builtin', '-fno-stack-protector', '-fno-pie', '-mgeneral-regs-only',
                '-fno-asynchronous-unwind-tables', '-ffixed-x18',
                '-c', ROOT/f'fastboot/{name}.c', '-o', ext/f'{name}.o')
    run(a.cross_compile+'ld', '-T', ROOT/'debug/layout.ld',
        *(f'--defsym={name}={value:#x}' for name, value in defs.items()),
        ext/'hook.o', *((ext/'screen.o',) if screen else ()),
        *((ext/'flash.o',ext/'download.o') if a.fastboot_flash else ()), '-o', ext/'hook.elf')
    run(a.cross_compile+'objcopy', '-O', 'binary', '-j', '.text', ext/'hook.elf', ext/'hook.bin')
    listing = subprocess.check_output([a.cross_compile+'nm', '-n', str(ext/'hook.elf')], text=True)
    symbols = {line.split()[2]:int(line.split()[0],16) for line in listing.splitlines() if len(line.split())==3}
    data = IMAGE['compose']((out/'ramlk/lk-jump-rmk.img').read_bytes(),
                             (ext/'hook.bin').read_bytes(), symbols,
                             watchdog=watchdog, usb_recovery=usb, screen=screen,
                             fastboot_flash=a.fastboot_flash, watchdog_ms=a.watchdog_ms)
    (out/'lk-debuger.img').write_bytes(data)
    for name, hang in (('ramboot-smoke', 0), ('ramboot-hang', 1)):
        run(a.cross_compile+'gcc', '-march=armv8-a', f'-DW202_DEBUG_HANG={hang}',
            '-c', ROOT/'debug/probe.S', '-o', ext/(name+'.o'))
        run(a.cross_compile+'ld', '-Ttext=0x86000000', '--entry=probe_start',
            ext/(name+'.o'), '-o', ext/(name+'.elf'))
        run(a.cross_compile+'objcopy', '-O', 'binary', '-j', '.text',
            ext/(name+'.elf'), out/(name+'.bin'))
    run(a.cross_compile+'gcc', '-march=armv8-a', '-c', ROOT/'debug/memory.S',
        '-o', ext/'memory-entry.o')
    run(a.cross_compile+'gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-std=c11',
        '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-pie',
        '-fno-asynchronous-unwind-tables', '-mgeneral-regs-only', '-ffixed-x18',
        '-c', ROOT/'debug/memory.c', '-o', ext/'memory.o')
    run(a.cross_compile+'ld', '-T', ROOT/'ramboot/probes/probe.ld',
        ext/'memory-entry.o', ext/'memory.o', '-o', ext/'ramboot-memory.elf')
    run(a.cross_compile+'objcopy', '-O', 'binary', ext/'ramboot-memory.elf', out/'ramboot-memory.bin')
    run(a.cross_compile+'gcc', '-march=armv8-a', '-c', ROOT/'debug/screen-probe.S',
        '-o', ext/'screen-probe.o')
    run(a.cross_compile+'ld', '-T', ROOT/'ramboot/probes/probe.ld',
        *(f'--defsym={name}={value:#x}' for name, value in defs.items() if name.startswith('lk_lcd_')),
        ext/'screen-probe.o', ext/'screen.o', '-o', ext/'ramboot-screen.elf')
    run(a.cross_compile+'objcopy', '-O', 'binary', ext/'ramboot-screen.elf', out/'ramboot-screen.bin')
    sources = [ROOT/'build.py', ROOT/'debug/hook.S', ROOT/'debug/layout.ld',
               ROOT/'debug/image.py', ROOT/'debug/probe.S', ROOT/'debug/memory.S', ROOT/'debug/memory.c',
               ROOT/'debug/screen.S', ROOT/'debug/screen-probe.S',
               ROOT/'ramboot/probes/probe.ld', ROOT/'ramlk/build.py', ROOT/'ramlk/hook.S']
    sources += [ROOT/'fastboot'/name for name in ('flash.c','download.c','flash.h','native.h')]
    report = {'schema':1, 'board':'w202ds', 'artifact':'lk-debuger.img',
              'bytes':len(data), 'sha256':hashlib.sha256(data).hexdigest(),
              'features':IMAGE['validate'](data), 'hardware_tested':False,
              'boot':'native Fastboot for normal/external reset' if usb else 'native LK mode dispatch',
              'ramboot':'PLK1 at 0x86000000; fastboot stage then oem jump-rmk :probe or :init',
              'watchdog_policy':'arm before a RAM payload; stop and restore monitor when it returns',
              'commands':commands, 'symbols':symbols,
              'probes':{name:{'bytes':(out/name).stat().st_size,
                              'sha256':hashlib.sha256((out/name).read_bytes()).hexdigest()}
                        for name in ('ramboot-smoke.bin','ramboot-hang.bin','ramboot-memory.bin','ramboot-screen.bin')},
              'source_sha256':{str(f.relative_to(ROOT)):hashlib.sha256(f.read_bytes()).hexdigest() for f in sources}}
    (out/'manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
