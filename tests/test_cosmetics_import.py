"""Catalog extraction/coverage tests; run python -m unittest discover -s tests -p test_cosmetics_import.py."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from import_cosmetics import build_catalog, emit_header, inventory, resolve_prefab
from import_weapon_names import keyvalues


class ImporterTests(unittest.TestCase):
    def sample(self):
        return dict(
            prefabs={
                'base': {'model_player': 'models/base.vmdl', 'flexible_loadout_group': 'rifle',
                         'item_name': '#Alpha', 'visuals': {'first': 'base', 'retained': 'yes'}},
                'child': {'prefab': 'base', 'visuals': {'first': 'child'}},
                'melee': {'flexible_loadout_group': 'melee', 'model_player': 'models/knife.vmdl',
                          'item_name': '#Knife'},
                'hands': {'flexible_loadout_group': 'clothing_hands', 'model_player': 'models/glove.vmdl',
                          'item_name': '#Gloves'},
            },
            items={
                '1': {'name': 'weapon_alpha', 'prefab': 'child'},
                '2': {'name': 'weapon_alpha_silencer', 'prefab': 'base', 'model_player': 'models/silencer.vmdl'},
                '500': {'name': 'weapon_knife_alpha', 'prefab': 'melee'},
                '5030': {'name': 'sporty_gloves', 'prefab': 'hands'},
                '999': {'name': 'crate', 'prefab': 'server_only_missing_prefab'},
            },
            paint_kits={
                '0': {'name': 'default', 'description_tag': '#default', 'wear_remap_min': '0.06', 'wear_remap_max': '0.8'},
                '1': {'name': 'red', 'description_tag': '#red', 'wear_remap_min': '0', 'wear_remap_max': '0.5', 'use_legacy_model': '1'},
                '2': {'name': 'blue', 'description_tag': '#blue'},
                '3': {'name': 'silencer_red', 'description_tag': '#red'},
            },
            item_sets={'set': {'items': {'[blue]weapon_alpha': '1'}}},
            client_loot_lists={'list': {'[red]weapon_alpha_silencer': '1'}},
        )

    def tokens(self):
        return {'alpha': 'Alpha', 'knife': 'Knife', 'gloves': 'Sport Gloves', 'default': 'Default',
                'red': 'Red', 'blue': 'Blue'}

    def test_duplicate_keyvalue_objects_are_merged(self):
        raw = b'"root" { "items" { "1" { "a" "old" "keep" "yes" } } "items" { "2" { "x" "new" } "1" { "a" "new" } } }'
        data = keyvalues(raw)['root']['items']
        self.assertEqual(data['1'], {'a': 'new', 'keep': 'yes'})
        self.assertEqual(data['2'], {'x': 'new'})

    def test_prefab_nested_overrides_and_retains(self):
        schema = self.sample()
        resolved = resolve_prefab(schema['items']['1'], schema['prefabs'])
        self.assertEqual(resolved['model_player'], 'models/base.vmdl')
        self.assertEqual(resolved['visuals'], {'first': 'child', 'retained': 'yes'})
        self.assertEqual(resolve_prefab(schema['items']['2'], schema['prefabs'])['model_player'], 'models/silencer.vmdl')

    def test_prefab_cycle_and_missing_are_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Cyclic'):
            resolve_prefab({'prefab': 'a'}, {'a': {'prefab': 'b'}, 'b': {'prefab': 'a'}})
        with self.assertRaisesRegex(ValueError, 'Missing'):
            resolve_prefab({'prefab': 'a'}, {})

    def test_exact_associations_not_cartesian_product(self):
        paths = ['panorama/images/econ/default_generated/weapon_alpha_silencer_red_light_png.vtex_c',
                 'panorama/images/econ/default_generated/weapon_alpha_silencer_red_medium_large_png.vtex_c']
        items, paints, pairs, coverage = build_catalog(self.sample(), self.tokens(), paths)
        associations = {(p['item_id'], p['paint_id']) for p in pairs}
        self.assertEqual(associations, {(1, 0), (1, 2), (2, 0), (2, 1), (500, 0), (5030, 0)})
        self.assertEqual(coverage['icon_pair_count'], 1)
        self.assertEqual(coverage['inventory_finish_icon_count'], 2)
        self.assertEqual({i['category'] for i in items}, {'weapon', 'knife', 'gloves'})
        self.assertEqual(paints[1]['legacy_model'], True)
        self.assertEqual(paints[1]['legacy_model_specified'], True)
        self.assertEqual(paints[2]['legacy_model_specified'], False)
        self.assertEqual((paints[2]['wear_min'], paints[2]['wear_max']), (0.06, 0.8))
        self.assertEqual(paints[2]['wear_min_explicit'], False)

    def test_unmatched_icon_does_not_invent_paint_id(self):
        paths = ['panorama/images/econ/default_generated/weapon_alpha_unknown_light_png.vtex_c']
        _, _, pairs, coverage = build_catalog(self.sample(), self.tokens(), paths)
        self.assertEqual(coverage['inventory_icons_with_no_schema_paint'], paths)
        self.assertFalse(any(p['paint_id'] not in (0, 1, 2, 3) for p in pairs))

    def test_unresolved_schema_finish_fails(self):
        schema = self.sample()
        schema['item_sets']['set']['items']['[unknown]weapon_alpha'] = '1'
        with self.assertRaisesRegex(ValueError, 'Unknown paint'):
            build_catalog(schema, self.tokens(), [])

    def test_invalid_wear_fails(self):
        schema = self.sample()
        schema['paint_kits']['1']['wear_remap_min'] = '0.8'
        with self.assertRaisesRegex(ValueError, 'Invalid wear'):
            build_catalog(schema, self.tokens(), [])

    def test_vpk_inventory_bounds_and_paths(self):
        tree = b'vtex_c\0panorama/images/econ/default_generated\0weapon_alpha_red_light_png\0'
        tree += struct.pack('<IHHIIH', 0, 0, 0x7fff, 0, 0, 0xffff) + b'\0\0\0'
        blob = struct.pack('<IIIIIII', 0x55AA1234, 2, len(tree), 0, 0, 0, 0) + tree
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'test_dir.vpk'
            path.write_bytes(blob)
            self.assertEqual(inventory(path), ['panorama/images/econ/default_generated/weapon_alpha_red_light_png.vtex_c'])
            path.write_bytes(blob[:-1])
            with self.assertRaisesRegex(ValueError, 'Truncated'):
                inventory(path)


@unittest.skipUnless((ROOT / 'analysis/cosmetics-source.json').is_file(),
                     'Local catalog evidence is not included in the source repository')
class InstalledCatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.report = json.loads((ROOT / 'analysis/cosmetics-source.json').read_text(encoding='utf-8'))
        cls.items = {item['id']: item for item in cls.report['items']}
        cls.paints = {paint['id']: paint for paint in cls.report['paints']}
        cls.pairs = {(pair['item_id'], pair['paint_id']) for pair in cls.report['compatible_pairs']}

    def test_all_real_schema_paints_have_supported_pair(self):
        used = {paint_id for _, paint_id in self.pairs}
        unused = [p['internal'] for p in self.report['paints'] if p['id'] not in used]
        self.assertEqual(unused, ['workshop_default'])
        self.assertGreater(len(used), 1400)
        for item_id in self.items:
            self.assertIn((item_id, 0), self.pairs)

    def test_known_knife_glove_and_gun_pairings(self):
        self.assertEqual(self.items[7]['model'], 'weapons/models/ak47/weapon_rif_ak47.vmdl')
        self.assertEqual(self.items[41]['category'], 'knife')
        self.assertEqual(self.items[4725]['category'], 'gloves')
        self.assertEqual(self.items[507]['category'], 'knife')
        self.assertIn((7, 180), self.pairs)      # AK-47 Fire Serpent.
        self.assertIn((507, 418), self.pairs)    # Karambit Doppler Phase1.
        self.assertIn((5030, 10045), self.pairs) # Sport Gloves Amphibious.
        self.assertNotIn((7, 10045), self.pairs)
        self.assertNotIn((5030, 180), self.pairs)

    def test_header_is_reproducible(self):
        report = self.report
        expected = emit_header(report['items'], report['paints'], report['compatible_pairs'])
        self.assertEqual((ROOT / 'src/generated/cosmetics.hpp').read_text(encoding='utf-8'), expected)

    def test_generated_cpp_api_compiles(self):
        compiler = ROOT / '.toolchain/llvm-mingw/bin/x86_64-w64-mingw32-clang++.exe'
        if not compiler.exists():
            self.skipTest('Local Windows compiler unavailable')
        source = '''#include "generated/cosmetics.hpp"
static_assert(cosmetics::find_item(7)->id == 7);
static_assert(cosmetics::find_item(4725)->category == cosmetics::Category::gloves);
static_assert(cosmetics::find_paint(418)->wear_max == 0.08f);
static_assert(cosmetics::find_paint(180)->legacy_model);
static_assert(cosmetics::compatible(507,418));
static_assert(!cosmetics::compatible(7,10045));
static_assert(!cosmetics::find_item(-1));
static_assert(!cosmetics::find_paint(999999));
int main() { return cosmetics::paints_for(*cosmetics::find_item(7)).empty(); }
'''
        result = subprocess.run([str(compiler), '-std=c++20', '-fsyntax-only', '-I' + str(ROOT / 'src'), '-x', 'c++', '-'],
                                input=source, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
