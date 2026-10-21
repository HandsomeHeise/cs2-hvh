"""Check the bounded decoder and model evidence used by native geometry selection."""
import hashlib
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import import_cosmetic_geometry as geometry
from import_weapon_names import assets


class GeometryTests(unittest.TestCase):
    def test_lz4_overlap(self):
        self.assertEqual(geometry.lz4(b'\x11a\x01\x00', 6), b'aaaaaa')

    def test_lz4_invalid_input_is_rejected(self):
        for content, expected in [(b'\x00\x01\x00', 4), (b'\x50a', 5), (b'\x10a', 2), (b'', 100000001)]:
            with self.subTest(content=content, expected=expected), self.assertRaises(ValueError):
                geometry.lz4(content, expected)

    def test_kv3_truncated_lane_is_rejected(self):
        lane = geometry.Lane(b'\x00')
        with self.assertRaises(ValueError):
            lane.get('I')

    @unittest.skipUnless((ROOT / 'analysis/cosmetic-legacy-assets.json').is_file(),
                         'Local model evidence is not included in the source repository')
    def test_generated_header_matches_retained_model_evidence(self):
        report = json.loads((ROOT / 'analysis/cosmetic-legacy-assets.json').read_text())
        self.assertEqual(geometry.generate_header(report['models']), (ROOT / 'src/generated/cosmetic_geometry.hpp').read_text())
        records = {record['id']: record for record in report['models']}
        for item_id in (9, 61):
            self.assertEqual((records[item_id]['modern_mask'], records[item_id]['legacy_mask']), (1, 2))
        self.assertEqual(records[2]['legacy_default_mask'], 10)
        self.assertFalse(records[31]['eligible'])

    @unittest.skipUnless((ROOT / 'analysis/cosmetic-legacy-assets.json').is_file(),
                         'Local model evidence is not included in the source repository')
    def test_installed_models_reproduce_geometry_and_hashes(self):
        report = json.loads((ROOT / 'analysis/cosmetic-legacy-assets.json').read_text())
        vpk = Path(report['vpk'])
        if not vpk.exists():
            self.skipTest('Installed VPK unavailable')
        models = report['models']
        content = assets(vpk, [model['resource'] for model in models])
        for model in models:
            with self.subTest(item=model['id']):
                resource = content[model['resource']]
                self.assertEqual(hashlib.sha256(resource).hexdigest(), model['sha256'])
                actual = geometry.geometry_record(model, resource)
                for key in ('eligible', 'modern_mask', 'legacy_mask', 'switch_mask', 'default_mask', 'model_data'):
                    self.assertEqual(actual[key], model[key])


if __name__ == '__main__':
    unittest.main()
