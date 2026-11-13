#!/usr/bin/env python3
"""Build W202DS UEFI partition firmware and independent ESP applications."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import runpy

ROOT = Path(__file__).resolve().parent
PATHS = runpy.run_path(str(ROOT/'tools/project_paths.py'))
CONTAINER = runpy.run_path(str(ROOT/'tools/lk_container.py'))
VERIFIER = runpy.run_path(str(ROOT/'tools/verify_firmware.py'))


def load_inputs(file):
    if file is None:
        return {'lk_baseline': None}
    config = json.loads(file.read_text())
    if config.get('schema') != 1 or config.get('board') != 'w202ds':
        raise ValueError('boot inputs must have schema=1 and board=w202ds')
    result = {}
    for name in ('lk_baseline',):
        value = config.get(name)
        if name == 'lk_baseline' and value is None:
            result[name] = None
            continue
        if not isinstance(value, str) or not value:
            raise ValueError('missing boot input: '+name)
        result[name] = (file.resolve().parent/value).resolve()
        if not result[name].is_file():
            raise ValueError('missing boot input file: '+str(result[name]))
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--stage', choices=['all', 'efi', 'uefi'], default='all')
    p.add_argument('--jobs', type=int, default=4)
    p.add_argument('--source-cache', type=Path)
    p.add_argument('--kernel-source', type=Path, help='compatibility argument; firmware defaults to repository inputs')
    p.add_argument('--inputs', type=Path, help='optional JSON override with schema, board and optional lk_baseline')
    p.add_argument('--lk-debug', action='store_true', help='enable watchdog and USB recovery in the generated minimal LK base')
    p.add_argument('--fastboot', action=argparse.BooleanOptionalAction, default=True,
                   help='polled LK Fastboot service while the UEFI menu owns the display')
    a = p.parse_args()
    if a.jobs < 1:
        p.error('jobs must be positive')
    out = a.output.resolve()
    platform = PATHS['default_repository']('edk2-ums9620', 'build.sh').resolve()
    runtime = PATHS['default_repository']('pj4edk2', 'build.sh').resolve()
    edk = platform/'edk2'
    for source in [ROOT.parent, platform, runtime]:
        if out == source or source in out.parents:
            p.error('output must be outside source repositories')
    if out.exists():
        p.error('choose a fresh output directory')
    tools = os.environ.get('EDK_TOOLS_PATH')
    if not tools:
        p.error('EDK_TOOLS_PATH must point to compiled EDK II BaseTools')
    packaging_inputs = {}
    lk = PATHS['lk_directory']()
    lk_generated = None
    if a.stage in ('all', 'uefi'):
        try:
            packaging_inputs = load_inputs(a.inputs)
            if packaging_inputs['lk_baseline'] is None or a.lk_debug:
                lk_generated = CONTAINER['build_ramlk'](debug=a.lk_debug)
            if packaging_inputs['lk_baseline'] is None:
                packaging_inputs['lk_baseline'] = lk_generated['image']
            packaging_inputs['lk_features'] = CONTAINER['validate_inputs'](
                lk, packaging_inputs['lk_baseline'],
                json.loads((ROOT/'inputs.lock.json').read_text()))
            if a.lk_debug:
                CONTAINER['require_debug_features'](packaging_inputs['lk_features'])
        except (ValueError, OSError) as error:
            p.error(str(error))
    out.mkdir(parents=True)
    debug_tools = CONTAINER['export_debug_tools'](lk, lk_generated['output'],
                                                  packaging_inputs['lk_baseline'], out) if a.lk_debug else []
    (out/'logs').mkdir()
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1', PJB_JOBS=str(a.jobs))
    env.update(W202_STORAGE_TEST='FALSE', W202_BOARD_SERVICES='TRUE', W202_TOUCH_TEST='TRUE',
               W202_LK_FASTBOOT='TRUE' if a.fastboot else 'FALSE',
               W202_TOUCH_DIRECT='FALSE', W202_TOUCH_RELOAD='TRUE', W202_ANDROID_BOOT_TEST='FALSE',
               W202_ANDROID_BOOT_ENTER='FALSE', W202_LK_RETURN_TEST='FALSE', W202_LK_RETURN_PROBE_ONLY='FALSE')
    partition_images = {}
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
        run('platform', 'bash', platform/'build.sh', '--target', 'w202ds', '--output', out/'platform', '--edk2', edk,
            '--abi-include', runtime/'pocketjsforedk2/include')
        sdk = out/'link-sdk'
        run('export-sdk', sys.executable, ROOT/'tools/edk2_sdk.py', 'export', '--board', 'w202ds',
            '--id', 'reimuks-w202ds', '--source', platform,
            '--fv', out/'platform/workspace/Build/Ums9620/RELEASE_GCC5/FV',
            '--support', out/'runtime/abi-support', '--config', ROOT/'config/pocketjs.json',
            '--tools', Path(tools)/'Source/C/bin', '--size', '0x200000', '--output', sdk)
        run('link', sys.executable, ROOT/'tools/edk2_sdk.py', 'link', '--sdk', sdk/'payload',
            '--archive', out/'runtime/PocketJS.a', '--work', out/'final-link', '--output', out/'w202ds.fd')
        run('ramboot', sys.executable, ROOT/'tools/mkramboot.py', '--fd', out/'w202ds.fd', '--output', out/'ramboot.img')
        partition_images = CONTAINER['package'](run, out, lk, platform,
                                                packaging_inputs['lk_baseline'], 'reimuks-w202ds', fastboot=a.fastboot)
    artifacts = list((out/'efi/bin').glob('*.efi')) + list((out/'ESP').rglob('*.efi')) + debug_tools
    artifacts += [f for f in [out/'w202ds.fd',out/'ramboot.img',out/'ramboot-uefi.bin',
                              out/'resident/uboot-b-uefi.img',out/'resident/manifest.json'] if f.is_file()]
    def identity(path):
        return {'commit': subprocess.check_output(['git','-C',str(path),'rev-parse','HEAD'], text=True).strip(),
                'dirty': bool(subprocess.check_output(['git','-C',str(path),'status','--porcelain'],text=True))}
    inputs = {str(path.relative_to(out)):json.loads(path.read_text()) for path in
              [out/'runtime/runtime-build.json',out/'platform/platform-build.json',out/'efi/efi-build.json']
              if path.is_file()}
    source_inputs = {name: {'bytes': path.stat().st_size, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                     for name, path in packaging_inputs.items() if isinstance(path, Path)} if partition_images else {}
    record = {'build_inputs':inputs,'builder_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'board':'w202ds','stage':a.stage,'hardware_tested':False,
              'partition_images':partition_images, 'packaging_inputs':source_inputs,
              'lk_base_features':packaging_inputs.get('lk_features'),
              'sources':{str(path):identity(path) for path in [ROOT.parent,platform,runtime]},
              'artifacts':{str(f.relative_to(out)):{'bytes':f.stat().st_size,
                 'sha256':hashlib.sha256(f.read_bytes()).hexdigest()} for f in sorted(artifacts)}}
    (out/'firmware-manifest.json').write_text(json.dumps(record,indent=2)+'\n')
    VERIFIER['verify'](out, 'firmware-manifest.json')
    print(out/'firmware-manifest.json')


if __name__ == '__main__':
    main()
