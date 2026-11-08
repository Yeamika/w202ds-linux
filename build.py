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
RUNTIME_SOURCES = ('edk2-libc', 'pocketjs', 'quickjs')


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


def verify_abi_sources(support, upstream, platform_edk2):
    if support['edk2'] != platform_edk2:
        raise SystemExit('Runtime ABI support EDK II differs from the W202DS platform EDK II')
    if support['libc'] != upstream['dependencies']['edk2-libc']['commit']:
        raise SystemExit('Runtime ABI support edk2-libc differs from the selected upstream')


def route_source_options(values, platform_names):
    platform, runtime = [], []
    for value in values:
        name, separator, _ = value.partition('=')
        if separator and name in (*platform_names, 'brotli'):
            platform.append(value)
        elif separator and name in RUNTIME_SOURCES:
            runtime.append(value)
        else:
            raise ValueError(f'Unknown upstream source selection: {value}')
    return platform, runtime


def verify_platform_sources(platform, expected):
    if platform['edk2'] != expected['dependencies']['edk2']['commit']:
        raise SystemExit('Platform build EDK II differs from the selected upstream')
    actual = platform.get('upstream_sources', {}).get('dependencies')
    for name, entry in expected['dependencies'].items():
        if actual:
            if any(actual[name][key] != entry[key] for key in ('repository', 'commit')):
                raise SystemExit(f'Platform build upstream differs: {name}')
        elif name != 'edk2' and entry['commit'] != entry['gitlink']:
            raise SystemExit('Legacy platform manifest cannot verify overridden Brotli sources; rebuild the platform')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--edk2', type=Path, default=ROOT.parent/'edk2-ums9620', help='platform repository')
    p.add_argument('--pocketjs', type=Path, default=ROOT.parent/'pj4edk2', help='runtime repository')
    p.add_argument('--default-ui', type=Path, help='external pj4edk2 UI directory containing ui.json')
    p.add_argument('--source-cache', type=Path, help='external upstream Git cache shared by build stages')
    p.add_argument('--source-lock', type=Path, help='resolved upstream.lock.json to replay')
    p.add_argument('--source-repo', action='append', default=[], metavar='NAME=URL')
    p.add_argument('--source-ref', action='append', default=[], metavar='NAME=REV',
                   help='edk2, brotli, edk2-libc, pocketjs or quickjs: commit, branch, tag or latest')
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
    platform_api = runpy.run_path(str(a.edk2/'tools/upstream.py'))
    platform_repos, runtime_repos = route_source_options(a.source_repo, platform_api['NAMES'])
    platform_refs, runtime_refs = route_source_options(a.source_ref, platform_api['NAMES'])
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
    platform_sources = a.output/'platform-sources'
    argv = [sys.executable, a.edk2/'tools/upstream.py', 'prepare',
            '--output', platform_sources, '--source-cache', source_cache]
    if a.source_lock:
        argv.extend(['--source-lock', a.source_lock])
    for option, values in [('--source-repo', platform_repos), ('--source-ref', platform_refs)]:
        for value in values:
            argv.extend([option, value])
    run('platform-upstream', *argv)
    platform_upstream = json.loads((platform_sources/'upstream.lock.json').read_text())
    edk2_source = Path(json.loads((platform_sources/'source-paths.json').read_text())['edk2'])
    selections = json.loads((a.pocketjs/'sources.lock.json').read_text())
    if a.source_lock:
        selections['dependencies'].update(json.loads(a.source_lock.read_text())['dependencies'])
    selections['dependencies'].update(platform_upstream['dependencies'])
    combined_inputs = a.output/'source-inputs.lock.json'
    combined_inputs.write_text(json.dumps(selections, indent=2)+'\n')
    argv = [sys.executable, a.pocketjs/'tools/upstream.py', 'resolve',
            '--output', source_lock, '--source-cache', source_cache, '--edk2', edk2_source,
            '--source-lock', combined_inputs]
    for option, values in [('--source-repo', runtime_repos), ('--source-ref', runtime_refs)]:
        for value in values:
            argv.extend([option, value])
    run('upstream', *argv)
    upstream_sources = json.loads(source_lock.read_text())
    upstream_sources['dependencies'].update(platform_upstream['dependencies'])
    source_lock.write_text(json.dumps(upstream_sources, indent=2)+'\n')
    runtime_inputs = runtime_api['identity'](a.pocketjs, a.default_ui, source_lock)

    tools = env.get('EDK_TOOLS_PATH')
    if tools:
        tools = Path(tools).resolve()
    else:
        tools = a.output/'basetools'
        run('basetools', 'bash', a.edk2/'build-tools.sh', '--output', tools, '--edk2', edk2_source)
    env['EDK_TOOLS_PATH'] = str(tools)
    runtime = a.runtime_build or a.output/'pocketjs'
    if not a.runtime_build:
        argv = ['bash', a.pocketjs/'build.sh', '--edk2', edk2_source, '--output', runtime]
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
    verify_abi_sources(support_manifest, upstream_sources,
                       upstream_sources['dependencies']['edk2']['commit'])
    if sha(runtime/'PocketJS.a') != support_manifest['runtime_sha256']:
        raise SystemExit('PocketJS.a differs from its ABI support manifest')
    platform = a.platform_build or a.output/'platform'
    env.update(W202_STORAGE_TEST='FALSE', W202_BOARD_SERVICES='TRUE', W202_TOUCH_TEST='TRUE',
               W202_TOUCH_DIRECT='FALSE', W202_TOUCH_RELOAD='TRUE', W202_ANDROID_BOOT_TEST='FALSE',
               W202_ANDROID_BOOT_ENTER='FALSE', W202_LK_RETURN_TEST='FALSE', W202_LK_RETURN_PROBE_ONLY='FALSE')
    if not a.platform_build:
        run('platform', 'bash', a.edk2/'build.sh', '--output', platform,
            '--edk2', edk2_source, '--abi-include', a.pocketjs/'pocketjsforedk2/include',
            '--touch-firmware', a.touch_firmware)
    run('verify-platform', sys.executable, a.edk2/'tools/platform-manifest.py', 'verify',
        '--build', platform, '--abi-include', a.pocketjs/'pocketjsforedk2/include',
        '--touch-firmware', a.touch_firmware)
    platform_manifest = json.loads((platform/'platform-build.json').read_text())
    verify_platform_sources(platform_manifest, platform_upstream)
    verify_abi_sources(support_manifest, upstream_sources, platform_manifest['edk2'])
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
    try:
        main()
    except ValueError as error:
        raise SystemExit(str(error)) from error
