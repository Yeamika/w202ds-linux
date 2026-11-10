#!/usr/bin/env python3
"""Build firmware FD and ESP applications, without LK packaging or device writes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--stage', choices=['all', 'efi', 'uefi'], default='all')
    p.add_argument('--jobs', type=int, default=4)
    p.add_argument('--source-cache', type=Path)
    p.add_argument('--kernel-source', type=Path, required=True)
    a = p.parse_args()
    if a.jobs < 1:
        p.error('jobs must be positive')
    out = a.output.resolve()
    platform = (ROOT/'components/edk2-ums9620').resolve()
    runtime = (ROOT/'components/pj4edk2').resolve()
    edk = platform/'edk2'
    for source in [ROOT.parent, platform, runtime]:
        if out == source or source in out.parents:
            p.error('output must be outside source repositories')
    if out.exists():
        p.error('choose a fresh output directory')
    tools = os.environ.get('EDK_TOOLS_PATH')
    if a.stage in ('all', 'uefi') and not (a.kernel_source/'Devices/touchscreen/firmware/Himax_firmware_guoxian.bin').is_file():
        p.error('selected kernel project is missing the pinned touchscreen firmware')
    if not tools:
        p.error('EDK_TOOLS_PATH must point to compiled EDK II BaseTools')
    out.mkdir(parents=True)
    (out/'logs').mkdir()
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PJB_JOBS=str(a.jobs))
    def run(stage, *argv):
        print(stage, flush=True)
        with (out/'logs'/(stage+'.log')).open('w') as log:
            result = subprocess.run([str(x) for x in argv], env=env, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            raise SystemExit(f'{stage} failed; see {out}/logs/{stage}.log')
    if a.stage in ('all', 'efi'):
        run('efi', 'bash', ROOT/'efi/build-efi.sh', '--output', out/'efi', '--edk2', edk,
            '--platform', platform, '--abi-include', runtime/'pocketjsforedk2/include',
            '--tools', tools, '--python', sys.executable, '--jobs', a.jobs)
        for folder, name in [('Android','AndroidLk'),('DebianServer','DebianServer'),('DebianKDE','DebianKde')]:
            dest = out/'ESP/EFI'/folder/'bootaa64.efi'
            dest.parent.mkdir(parents=True)
            dest.write_bytes((out/'efi/bin'/(name+'.efi')).read_bytes())
        for name in ['AndroidBoot','HandoffProbe','TouchScope']:
            dest = out/'ESP/EFI/Tools'/(name+'.efi')
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes((out/'efi/bin'/(name+'.efi')).read_bytes())
    if a.stage in ('all', 'uefi'):
        argv = ['bash', runtime/'build.sh', '--output', out/'runtime', '--edk2', edk]
        if a.source_cache:
            argv += ['--source-cache', a.source_cache.resolve()]
        run('runtime', *argv)
        firmware = a.kernel_source.resolve()/'Devices/touchscreen/firmware/Himax_firmware_guoxian.bin'
        run('platform', 'bash', platform/'build.sh', '--output', out/'platform', '--edk2', edk,
            '--abi-include', runtime/'pocketjsforedk2/include', '--touch-firmware', firmware)
        sdk = out/'link-sdk'
        run('export-sdk', sys.executable, ROOT/'tools/edk2_sdk.py', 'export', '--board', 'w202ds',
            '--id', 'reimuks-w202ds', '--source', platform,
            '--fv', out/'platform/workspace/Build/Ums9620/RELEASE_GCC5/FV',
            '--support', out/'runtime/abi-support', '--config', ROOT/'config/pocketjs.json',
            '--tools', Path(tools)/'Source/C/bin', '--size', '0x200000', '--output', sdk)
        run('link', sys.executable, ROOT/'tools/edk2_sdk.py', 'link', '--sdk', sdk/'payload',
            '--archive', out/'runtime/PocketJS.a', '--work', out/'final-link', '--output', out/'w202ds.fd')
        run('ramboot', sys.executable, ROOT/'tools/mkramboot.py', '--fd', out/'w202ds.fd', '--output', out/'ramboot.img')
    artifacts = list((out/'efi/bin').glob('*.efi')) + list((out/'ESP').rglob('*.efi'))
    artifacts += [f for f in [out/'w202ds.fd',out/'ramboot.img'] if f.is_file()]
    def identity(path):
        return {'commit': subprocess.check_output(['git','-C',str(path),'rev-parse','HEAD'], text=True).strip(),
                'dirty': bool(subprocess.check_output(['git','-C',str(path),'status','--porcelain'],text=True))}
    inputs = {str(path.relative_to(out)):json.loads(path.read_text()) for path in
              [out/'runtime/runtime-build.json',out/'platform/platform-build.json',out/'efi/efi-build.json']
              if path.is_file()}
    record = {'build_inputs':inputs,'builder_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'board':'w202ds','stage':a.stage,'hardware_tested':False,
              'sources':{str(path):identity(path) for path in [ROOT.parent,platform,runtime]},
              'artifacts':{str(f.relative_to(out)):{'bytes':f.stat().st_size,
                 'sha256':hashlib.sha256(f.read_bytes()).hexdigest()} for f in sorted(artifacts)}}
    (out/'firmware-manifest.json').write_text(json.dumps(record,indent=2)+'\n')
    print(out/'firmware-manifest.json')


if __name__ == '__main__':
    main()
