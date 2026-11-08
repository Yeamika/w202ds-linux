#!/usr/bin/env python3
"""Build W202DS UEFI + PocketJS and package a resident LK B-slot container."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def repository_identity(root):
    def git(*args):
        return subprocess.check_output(['git', '-C', str(root), *args], text=True)
    files = git('ls-files', '--cached', '--others', '--exclude-standard', '-z').split('\0')
    return {'commit': git('rev-parse', 'HEAD').strip(),
            'origin': git('remote', 'get-url', 'origin').strip(),
            'submodules': git('submodule', 'status', '--recursive').splitlines(),
            'source_files': {name: sha(root/name) for name in sorted(set(files))
                             if name and (root/name).is_file()}}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--edk2', type=Path, default=ROOT.parent/'edk2-ums9620', help='platform repository')
    p.add_argument('--pocketjs', type=Path, default=ROOT.parent/'pj4edk2', help='runtime repository')
    p.add_argument('--default-ui', type=Path, help='external pj4edk2 UI directory containing ui.json')
    p.add_argument('--source-cache', type=Path, help='external upstream Git cache shared by build stages')
    p.add_argument('--source-lock', type=Path, help='resolved upstream.lock.json to replay')
    p.add_argument('--source-repo', action='append', default=[], metavar='NAME=URL')
    p.add_argument('--source-ref', action='append', default=[], metavar='NAME=REV',
                   help='edk2-libc, pocketjs or quickjs: commit, branch, tag or latest')
    p.add_argument('--output', type=Path, required=True, help='fresh directory outside all source trees')
    p.add_argument('--lk-baseline', type=Path, required=True)
    p.add_argument('--settings-image', type=Path, required=True)
    p.add_argument('--touch-firmware', type=Path, required=True)
    p.add_argument('--rust-build', type=Path, help='verified Rust-only bundle from a compiler machine')
    p.add_argument('--runtime-build', type=Path, help='existing pj4edk2 build, including PocketJS.a and abi-support')
    p.add_argument('--platform-build', type=Path, help='existing edk2-ums9620 platform build')
    p.add_argument('--jobs', type=int, default=4)
    p.add_argument('--revision', default='w202ds-edk2-split')
    a = p.parse_args()
    for name in ('edk2', 'pocketjs', 'output', 'lk_baseline', 'settings_image', 'touch_firmware',
                 'rust_build', 'runtime_build', 'platform_build', 'default_ui', 'source_cache', 'source_lock'):
        value = getattr(a, name)
        if value is not None:
            setattr(a, name, value.resolve())
    if a.jobs < 1:
        p.error('--jobs must be positive')
    for source in (ROOT, a.edk2, a.pocketjs):
        if a.output == source or source in a.output.parents:
            p.error('--output must be outside all three source repositories')
    if a.output.exists():
        p.error('output exists; choose a fresh directory')
    runtime_api = runpy.run_path(str(a.pocketjs/'tools/rust-bundle.py'))
    # Resolve the same default as build.sh, including direct Cargo integration.
    a.default_ui = (a.default_ui or Path(os.environ.get('PJB_DEFAULT_UI') or a.pocketjs/'default-ui')).resolve()
    if a.output == a.default_ui or a.default_ui in a.output.parents:
        p.error('--output must be outside the UI directory')
    lock = json.loads((ROOT/'inputs.lock.json').read_text())
    for name in ('lk_baseline', 'touch_firmware'):
        file, expected = getattr(a, name), lock[name]
        if file.stat().st_size != expected['bytes'] or sha(file) != expected['sha256']:
            p.error(f'incorrect {name}: {file}')
    if a.settings_image.stat().st_size != 0x500000:
        p.error('settings source must be a complete compatible 5 MiB image')
    a.output.mkdir(parents=True)
    env = dict(os.environ, PJB_JOBS=str(a.jobs), PYTHONDONTWRITEBYTECODE='1')
    logs = a.output/'logs'
    logs.mkdir()

    def run(label, *argv, cwd=ROOT):
        print(f'{label} ...', flush=True)
        with (logs/(label+'.log')).open('w') as stream:
            r = subprocess.run([str(x) for x in argv], cwd=cwd, env=env,
                               stdout=stream, stderr=subprocess.STDOUT)
        if r.returncode:
            raise SystemExit(f'{label} failed ({r.returncode}); see {logs/(label+".log")}')

    source_lock = a.output/'upstream.lock.json'
    source_cache = (a.source_cache or Path(env.get('PJB_SOURCE_CACHE') or a.output/'source-cache')).resolve()
    for source in (ROOT, a.edk2, a.pocketjs, a.default_ui):
        if source_cache == source or source in source_cache.parents:
            p.error('--source-cache must be outside source and UI directories')
    argv = [sys.executable, a.pocketjs/'tools/upstream.py', 'resolve',
            '--output', source_lock, '--source-cache', source_cache]
    if a.source_lock:
        argv.extend(['--source-lock', a.source_lock])
    for option, values in [('--source-repo', a.source_repo), ('--source-ref', a.source_ref)]:
        for value in values:
            argv.extend([option, value])
    run('upstream', *argv)
    upstream_sources = json.loads(source_lock.read_text())
    runtime_inputs = runtime_api['identity'](a.pocketjs, a.default_ui, source_lock)

    tools = env.get('EDK_TOOLS_PATH')
    if tools:
        tools = Path(tools).resolve()
    else:
        tools = a.output/'basetools'
        run('basetools', 'bash', a.edk2/'build-tools.sh', tools)
    env['EDK_TOOLS_PATH'] = str(tools)
    runtime = a.runtime_build or a.output/'pocketjs'
    if not a.runtime_build:
        argv = ['bash', a.pocketjs/'build.sh', '--edk2', a.edk2/'edk2', '--output', runtime]
        argv.extend(['--default-ui', a.default_ui])
        argv.extend(['--source-lock', source_lock, '--source-cache', source_cache])
        if a.rust_build:
            argv.extend(['--rust-build', a.rust_build])
        run('pocketjs', *argv)
    support = runtime/'abi-support'
    runtime_identity = json.loads((runtime/'runtime-build.json').read_text())
    if (any(runtime_identity.get(key) != value for key, value in runtime_inputs.items())
            or runtime_identity['archive_sha256'] != sha(runtime/'PocketJS.a')
            or runtime_identity['abi_manifest_sha256'] != sha(support/'manifest.json')
            or runtime_identity.get('cargo_lock_sha256') != sha(runtime/'Cargo.lock')):
        raise SystemExit('Runtime build differs from its current sources, selected UI or recorded artifacts')
    support_manifest = json.loads((support/'manifest.json').read_text())
    pins = dict(json.loads((a.pocketjs/'sources.lock.json').read_text())['dependencies'],
                **upstream_sources['dependencies'])
    for name in ('edk2', 'edk2-libc'):
        key = 'libc' if name == 'edk2-libc' else name
        if support_manifest[key] != pins[name]['commit']:
            raise SystemExit(f'ABI support {name} pin differs')
    if sha(runtime/'PocketJS.a') != support_manifest['runtime_sha256']:
        raise SystemExit('PocketJS.a differs from its ABI support manifest')
    platform = a.platform_build or a.output/'platform'
    env.update(W202_STORAGE_TEST='FALSE', W202_BOARD_SERVICES='TRUE', W202_TOUCH_TEST='TRUE',
               W202_TOUCH_DIRECT='FALSE', W202_TOUCH_RELOAD='TRUE', W202_ANDROID_BOOT_TEST='FALSE',
               W202_ANDROID_BOOT_ENTER='FALSE', W202_LK_RETURN_TEST='FALSE', W202_LK_RETURN_PROBE_ONLY='FALSE')
    if not a.platform_build:
        run('platform', 'bash', a.edk2/'build.sh', '--output', platform,
            '--abi-include', a.pocketjs/'pocketjsforedk2/include', '--touch-firmware', a.touch_firmware)
    run('verify-platform', sys.executable, a.edk2/'tools/platform-manifest.py', 'verify',
        '--build', platform, '--abi-include', a.pocketjs/'pocketjsforedk2/include',
        '--touch-firmware', a.touch_firmware)
    build = platform/'workspace/Build/Ums9620/RELEASE_GCC5'
    sdk = a.output/'link-sdk'
    run('export-sdk', sys.executable, ROOT/'tools/edk2_sdk.py', 'export', '--board', 'w202ds',
        '--id', a.revision, '--source', a.edk2, '--fv', build/'FV', '--support', support,
        '--config', ROOT/'config/pocketjs.json', '--tools', tools/'Source/C/bin',
        '--size', '0x200000', '--output', sdk)
    run('link', sys.executable, ROOT/'tools/edk2_sdk.py', 'link', '--sdk', sdk/'payload',
        '--archive', runtime/'PocketJS.a', '--work', a.output/'final-link', '--output', a.output/'w202ds.fd')
    run('ramboot', sys.executable, ROOT/'tools/mkramboot.py', '--fd', a.output/'w202ds.fd',
        '--output', a.output/'ramboot.img')
    bridge = a.output/'lk-bridge'
    lk = ROOT/'lk/ramboot'
    run('lk-bridge', 'make', '-C', lk, f'OUT={bridge}', 'LK_RETURN=1',
        f'HANDOFF_INCLUDE={a.edk2/"Ums9620Pkg/Include"}', 'resident', 'enter-sec')
    resident = a.output/'resident'
    args = ['--baseline', a.lk_baseline, '--settings-image', a.settings_image,
            '--loader', bridge/'ramboot-resident.bin', '--image', a.output/'ramboot.img',
            '--output', resident, '--revision', a.revision]
    run('prepare-resident', sys.executable, lk/'package-resident.py', 'prepare', *args)
    run('lk-bootstrap', 'make', '-C', lk, f'OUT={resident}', 'resident-bootstrap')
    run('package-resident', sys.executable, lk/'package-resident.py', 'package', *args)
    for folder, name in [('Android', 'AndroidLk'), ('DebianServer', 'DebianServer'), ('DebianKDE', 'DebianKde')]:
        destination = a.output/'ESP/EFI'/folder/'bootaa64.efi'
        destination.parent.mkdir(parents=True)
        shutil.copy2(build/'AARCH64'/(name+'.efi'), destination)
    artifacts = [a.output/'w202ds.fd', a.output/'ramboot.img', resident/'uboot-b-uefi.img',
                 *sorted((a.output/'ESP').rglob('*.efi'))]
    manifest = {'schema': 1, 'revision': a.revision, 'device_validation': 'not-tested',
                'source_repositories': {name: repository_identity(path) for name, path in
                                        [('edk2-ums9620', a.edk2), ('pj4edk2', a.pocketjs), ('w202ds-boot', ROOT)]},
                'inputs': {name: {'bytes': getattr(a, name).stat().st_size, 'sha256': sha(getattr(a, name))}
                           for name in ('lk_baseline', 'settings_image', 'touch_firmware')},
                'runtime_sha256': sha(runtime/'PocketJS.a'),
                'upstream_sources': upstream_sources,
                'default_ui': runtime_inputs['default_ui'],
                'artifacts': {str(f.relative_to(a.output)): {'bytes': f.stat().st_size, 'sha256': sha(f)} for f in artifacts}}
    (a.output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(json.dumps({'manifest': str(a.output/'manifest.json'), 'revision': a.revision,
                      'artifacts': manifest['artifacts'], 'device_validation': 'not-tested'}, indent=2))


if __name__ == '__main__':
    main()
