"""Fetch upstream prebuilt spd_dump for the current platform."""
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import shutil
import tempfile
from urllib.request import Request, urlopen
from zipfile import ZipFile

REPO = 'TomKing062/action_spd_dump_it'


def fetch(url):
    with urlopen(Request(url, headers={'User-Agent': 'w202ds-boot'}), timeout=60) as response:
        return response.read()


def ensure_tool(root):
    system, machine = platform.system(), platform.machine().lower()
    if system == 'Windows':
        folder, exe = 'win', 'spd_dump.exe'
        arch = 'x64' if machine in ('amd64', 'x86_64') else 'x86'
        if machine not in ('amd64', 'x86_64', 'x86', 'i386', 'i686'):
            raise RuntimeError('unsupported Windows architecture: ' + machine)
        suffix = '_' + arch + '_Release'
    elif system == 'Linux':
        folder, exe = 'linux', 'spd_dump'
        arch = {'x86_64': 'x86_64', 'amd64': 'x86_64', 'aarch64': 'aarch64', 'arm64': 'aarch64'}.get(machine)
        if not arch:
            raise RuntimeError('unsupported Linux architecture: ' + machine)
        suffix = '_' + arch + '-unknown-linux-musl'
    else:
        raise RuntimeError('supported platforms: Windows and Linux')
    target = root / folder
    binary = target / exe
    marker = target / 'download.json'
    if binary.is_file() and marker.is_file():
        info = json.loads(marker.read_text())
        if info.get('architecture') == arch and info.get('binary_sha256') == hashlib.sha256(binary.read_bytes()).hexdigest():
            return binary
    payload = json.loads(fetch('https://api.github.com/repos/' + REPO + '/actions/artifacts?per_page=100'))
    artifacts = [a for a in payload['artifacts'] if not a['expired'] and
                 a['name'].startswith('spd_dump_it_main_') and a['name'].endswith(suffix)]
    if not artifacts:
        raise RuntimeError('no matching upstream artifact; pass --tool')
    artifact = max(artifacts, key=lambda a: a['created_at'])
    url = 'https://nightly.link/' + REPO + '/actions/artifacts/' + str(artifact['id']) + '.zip'
    data = fetch(url)
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=target.parent, prefix='download-') as tmp:
        stage = Path(tmp)
        with ZipFile(io.BytesIO(data)) as archive:
            candidates = [n for n in archive.namelist() if Path(n).name == exe or
                          (system == 'Linux' and Path(n).name.startswith('spd_dump-main-'))]
            if len(candidates) != 1:
                raise RuntimeError('unexpected executable layout in upstream archive')
            (stage / exe).write_bytes(archive.read(candidates[0]))
            for member in archive.namelist():
                if member.lower().endswith('.dll'):
                    (stage / Path(member).name).write_bytes(archive.read(member))
        if os.name != 'nt':
            (stage / exe).chmod(0o755)
        info = {'repository': REPO, 'artifact_id': artifact['id'], 'artifact_name': artifact['name'],
                'architecture': arch, 'archive_sha256': hashlib.sha256(data).hexdigest(),
                'binary_sha256': hashlib.sha256((stage / exe).read_bytes()).hexdigest()}
        (stage / 'download.json').write_text(json.dumps(info, indent=2) + '\n')
        target.mkdir(parents=True, exist_ok=True)
        for item in stage.iterdir():
            shutil.copy2(item, target / item.name)
    return binary
