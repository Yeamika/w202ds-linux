"""Check LK packaging contracts and corruption with recomputed outer hashes."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
from verify_firmware import verify


class BootPartitionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        if os.environ.get('W202_FIRMWARE_BUILD'):
            source = Path(os.environ['W202_FIRMWARE_BUILD'])
            manifest = source/'firmware-manifest.json'
            if not manifest.is_file():
                manifest = source/'manifest.json'
            self.record = json.loads(manifest.read_text())
            # Only partition packaging is tested here, on copies of the inputs.
            self.record['artifacts'] = {k: v for k, v in self.record['artifacts'].items() if k.startswith('resident/')}
            for name in self.record['artifacts']:
                dest = self.directory/name
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source/name, dest)
        else:
            self.make_metadata_fixture()
        self.save()

    def make_metadata_fixture(self):
        # A contract fixture, not a bootable firmware image or a hardware test.
        start, bundle = 0x228000, bytes(range(256)) * 2
        data = bytearray(0x500000)
        data[start:start+len(bundle)] = bundle
        image = self.directory/'resident/uboot-b-uefi.img'
        image.parent.mkdir()
        image.write_bytes(data)
        digest = hashlib.sha256(data).hexdigest()
        report = {'bytes': len(data), 'sha256': digest,
                  'settings_storage': 'esp-files', 'persistent_logging': False,
                  'bundle_sha256': hashlib.sha256(bundle).hexdigest(),
                  'layout': {'RO_FLASH_OFFSET': start, 'IMAGE_BYTES': len(bundle)}}
        (self.directory/'resident/manifest.json').write_text(json.dumps(report))
        self.record = {'board': 'w202ds', 'stage': 'uefi',
                       'partition_images': {'uboot_b': {
                           'path': 'resident/uboot-b-uefi.img', 'bytes': len(data),
                           'sha256': digest, 'format': 'w202ds-lk-resident'}},
                       'artifacts': {}}
        self.rehash_partition()

    def save(self):
        (self.directory/'firmware-manifest.json').write_text(json.dumps(self.record))

    def test_partition_and_packaging_report_agree(self):
        result = verify(self.directory, 'firmware-manifest.json')
        self.assertEqual(result['resident/uboot-b-uefi.img']['bytes'], 0x500000)

    def test_uefi_manifest_cannot_omit_partition(self):
        del self.record['partition_images']
        self.save()
        with self.assertRaisesRegex(ValueError, 'uboot_b partition image'):
            verify(self.directory, 'firmware-manifest.json')

    def rehash_partition(self):
        name = 'resident/uboot-b-uefi.img'
        path = self.directory/name
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        self.record['artifacts'][name] = {'bytes': path.stat().st_size, 'sha256': digest}
        self.record['partition_images']['uboot_b']['sha256'] = digest
        report_name = 'resident/manifest.json'
        report_path = self.directory/report_name
        report = json.loads(report_path.read_text())
        report['sha256'] = digest
        report_path.write_text(json.dumps(report))
        self.record['artifacts'][report_name] = {'bytes': report_path.stat().st_size,
                                               'sha256': hashlib.sha256(report_path.read_bytes()).hexdigest()}
        self.save()

    def test_modified_bundle_is_detected_even_with_updated_outer_hashes(self):
        report = json.loads((self.directory/'resident/manifest.json').read_text())
        path = self.directory/'resident/uboot-b-uefi.img'
        data = bytearray(path.read_bytes())
        data[report['layout']['RO_FLASH_OFFSET']] ^= 1
        path.write_bytes(data)
        self.rehash_partition()
        with self.assertRaisesRegex(ValueError, 'embedded bundle'):
            verify(self.directory, 'firmware-manifest.json')

    def test_unused_tail_is_zero_even_with_updated_outer_hashes(self):
        path = self.directory/'resident/uboot-b-uefi.img'
        data = bytearray(path.read_bytes())
        data[-1] = 1
        path.write_bytes(data)
        self.rehash_partition()
        with self.assertRaisesRegex(ValueError, 'unused partition tail'):
            verify(self.directory, 'firmware-manifest.json')


if __name__ == '__main__':
    unittest.main()
