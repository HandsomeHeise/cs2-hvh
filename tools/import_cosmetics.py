"""Import item models, localized finishes and exact pairings from installed CS2 VPK data.

Only the item schema/localization asset bytes are extracted in memory. Inventory icon
paths provide explicit item/paint associations, including unusual knives and gloves
whose server loot lists are absent from the shipped client schema. Item-set and client
loot-list keys supplement and corroborate those pairs. No game binary is modified.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import struct

from import_weapon_names import assets, keyvalues

ROOT = Path(__file__).resolve().parents[1]
ITEMS_ASSET = 'scripts/items/items_game.txt'
LANG_ASSET = 'resource/csgo_english.txt'


def inventory(directory):
    """Bounded VPK v1/v2 directory reader. Return paths without opening archives."""
    data = directory.read_bytes()
    if len(data) < 12:
        raise ValueError('Truncated VPK header')
    magic, version, size = struct.unpack_from('<III', data)
    if magic != 0x55AA1234 or version not in (1, 2):
        raise ValueError('Unsupported VPK header')
    header = 28 if version == 2 else 12
    limit = header + size
    if limit > len(data):
        raise ValueError('Truncated VPK directory')
    cursor = header

    def string():
        nonlocal cursor
        end = data.find(b'\0', cursor, limit)
        if end == -1:
            raise ValueError('Unterminated VPK string')
        value = data[cursor:end].decode('utf-8')
        cursor = end + 1
        return value

    paths = []
    while extension := string():
        while folder := string():
            while name := string():
                if cursor + 18 > limit:
                    raise ValueError('Truncated VPK entry')
                _, preload, _, _, _, terminator = struct.unpack_from('<IHHIIH', data, cursor)
                cursor += 18
                if terminator != 0xFFFF or cursor + preload > limit:
                    raise ValueError('Malformed VPK entry')
                cursor += preload
                paths.append(('' if folder == ' ' else folder + '/') + name +
                             ('' if extension == ' ' else '.' + extension))
    if cursor != limit:
        raise ValueError('Unexpected bytes after VPK tree')
    if len(paths) != len(set(paths)):
        raise ValueError('Duplicate VPK path')
    return paths


def merge(base, value):
    result = dict(base)
    for key, entry in value.items():
        if isinstance(result.get(key), dict) and isinstance(entry, dict):
            result[key] = merge(result[key], entry)
        else:
            result[key] = entry
    return result


def resolve_prefab(item, prefabs, visiting=()):
    result = {}
    for name in item.get('prefab', '').split():
        if name in visiting:
            raise ValueError('Cyclic prefab: ' + ' -> '.join((*visiting, name)))
        if name not in prefabs:
            raise ValueError('Missing prefab: ' + name)
        result = merge(result, resolve_prefab(prefabs[name], prefabs, (*visiting, name)))
    return merge(result, item)


def localized(token, tokens):
    return tokens.get(token.lstrip('#').lower(), '')


def paint_label(paint, tokens):
    label = localized(paint.get('description_tag', ''), tokens)
    if not label:
        label = paint['name'].replace('_', ' ').title()
    # These internal names distinguish finishes sharing the same localization token.
    name = paint['name']
    phase = re.search(r'(?:gamma_)?doppler_phase([1-4])(?:_|$)', name)
    variant = None
    if phase:
        variant = 'Phase ' + phase.group(1)
    else:
        for marker, value in (('ruby', 'Ruby'), ('sapphire', 'Sapphire'),
                              ('blackpearl', 'Black Pearl'), ('emerald', 'Emerald')):
            if name.startswith('am_' + marker + '_marbleized'):
                variant = value
                break
    return f'{label} ({variant})' if variant else label


def build_catalog(schema, tokens, paths):
    prefabs = schema.get('prefabs', {})

    def hands_candidate(value, seen=()):
        if value.get('flexible_loadout_group') == 'clothing_hands':
            return True
        return any(name not in seen and hands_candidate(prefabs.get(name, {}), (*seen, name))
                   for name in value.get('prefab', '').split())

    items = []
    for raw_id, value in schema['items'].items():
        if not raw_id.isdigit() or not 0 < int(raw_id) <= 65535 or not isinstance(value, dict):
            continue
        name = value.get('name', '')
        if not name.startswith('weapon_') and not hands_candidate(value):
            continue
        resolved = resolve_prefab(value, prefabs)
        if resolved.get('flexible_loadout_group') == 'clothing_hands':
            category = 'gloves'
        elif name.startswith('weapon_'):
            category = 'knife' if resolved.get('flexible_loadout_group') == 'melee' else 'weapon'
        else:
            continue
        label = localized(resolved.get('item_name', ''), tokens)
        if not label:
            raise ValueError('Missing localized item name: ' + name)
        if name in {'weapon_knife', 'weapon_knife_t', 'weapon_knifegg'}:
            label += {'weapon_knife': ' (CT)', 'weapon_knife_t': ' (T)', 'weapon_knifegg': ' (Gun Game)'}[name]
        model = resolved.get('model_player', '')
        if not model:
            raise ValueError('Missing model_player: ' + name)
        items.append(dict(id=int(raw_id), name=label, model=model, category=category, canonical=name))
    items.sort(key=lambda item: item['id'])
    item_by_name = {item['canonical']: item for item in items}
    if len(item_by_name) != len(items):
        raise ValueError('Duplicate canonical item name')

    raw_paints = schema['paint_kits']
    defaults = raw_paints['0']
    paints = []
    for raw_id, paint in raw_paints.items():
        if not raw_id.isdigit() or not isinstance(paint, dict):
            raise ValueError('Invalid paint entry')
        low = float(paint.get('wear_remap_min', defaults['wear_remap_min']))
        high = float(paint.get('wear_remap_max', defaults['wear_remap_max']))
        if not 0 <= low <= high <= 1:
            raise ValueError('Invalid wear range: ' + raw_id)
        legacy = paint.get('use_legacy_model')
        if legacy not in (None, '0', '1'):
            raise ValueError('Unknown legacy model value: ' + raw_id)
        paints.append(dict(id=int(raw_id), name=paint_label(paint, tokens), internal=paint['name'],
                           wear_min=low, wear_max=high, wear_min_explicit='wear_remap_min' in paint,
                           wear_max_explicit='wear_remap_max' in paint,
                           legacy_model=legacy == '1', legacy_model_specified=legacy is not None))
    paints.sort(key=lambda paint: paint['id'])
    paint_by_name = {paint['internal']: paint for paint in paints}
    if len(paint_by_name) != len(paints):
        raise ValueError('Duplicate internal paint name')

    evidence = defaultdict(lambda: defaultdict(set))
    unmatched_icons = []
    unresolved_paint_icons = []
    icon_count = 0
    names_longest = sorted(item_by_name, key=lambda name: (-len(name), name))
    icon_re = re.compile(r'panorama/images/econ/default_generated/(.+)_(light|medium|heavy)(?:_large)?_png\.vtex_c')
    for path in paths:
        match = icon_re.fullmatch(path)
        if not match:
            continue
        icon_count += 1
        stem = match.group(1)
        item_name = next((name for name in names_longest if stem.startswith(name + '_')), None)
        if not item_name:
            unmatched_icons.append(path)
            continue
        paint_name = stem[len(item_name) + 1:]
        if paint_name not in paint_by_name:
            unresolved_paint_icons.append(path)
            continue
        item_id, paint_id = item_by_name[item_name]['id'], paint_by_name[paint_name]['id']
        evidence[item_id][paint_id].add('inventory_icon')

    unmatched_schema_refs = []
    ref_count = 0
    ref_re = re.compile(r'\[([^]]+)\](.+)')

    def references(node, origin):
        nonlocal ref_count
        if not isinstance(node, dict):
            return
        for key, value in node.items():
            match = ref_re.fullmatch(key)
            if match and match.group(2) in item_by_name:
                ref_count += 1
                paint_name, item_name = match.groups()
                if paint_name not in paint_by_name:
                    unmatched_schema_refs.append(key)
                else:
                    evidence[item_by_name[item_name]['id']][paint_by_name[paint_name]['id']].add(origin)
            references(value, origin)

    references(schema.get('item_sets', {}), 'item_set')
    references(schema.get('client_loot_lists', {}), 'client_loot_list')
    if unmatched_schema_refs:
        raise ValueError('Unknown paint in item set/client loot reference: ' + repr(unmatched_schema_refs[:10]))

    for item in items:
        evidence[item['id']][0].add('default')
    pairs = [{'item_id': item_id, 'paint_id': paint_id, 'sources': sorted(sources)}
             for item_id, item_evidence in sorted(evidence.items())
             for paint_id, sources in sorted(item_evidence.items())]
    return items, paints, pairs, dict(
        inventory_finish_icon_count=icon_count,
        inventory_finish_icons_unmatched=sorted(unmatched_icons),
        inventory_icons_with_no_schema_paint=sorted(unresolved_paint_icons),
        item_set_and_client_loot_reference_count=ref_count,
        schema_pair_count=sum('item_set' in p['sources'] or 'client_loot_list' in p['sources'] for p in pairs),
        icon_pair_count=sum('inventory_icon' in p['sources'] for p in pairs),
        schema_only_pairs=[p for p in pairs if 'inventory_icon' not in p['sources'] and 'default' not in p['sources']],
    )


def cpp_string(value):
    return json.dumps(value)


def cpp_float(value):
    return repr(float(value)) + 'f'


def emit_header(items, paints, pairs):
    paint_lists = defaultdict(list)
    for pair in pairs:
        paint_lists[pair['item_id']].append(pair['paint_id'])
    lines = ['#pragma once', '#include <cstdint>', '#include <span>',
             '// Generated from installed game data; see analysis/cosmetics-source.json.',
             '// A paint absent use_legacy_model flag is recorded as unspecified, not a verified runtime default.',
             '// Missing wear remaps use the schema paint0 values and retain explicitness flags.',
             'namespace cosmetics {',
             'enum class Category { weapon, knife, gloves };',
             'struct Item { std::uint16_t id; const char* name; const char* model; Category category;',
             '    std::uint32_t paint_first; std::uint16_t paint_count; const char* canonical; };',
             'struct Paint { int id; const char* name; float wear_min, wear_max; const char* internal;',
             '    bool legacy_model, legacy_model_specified, wear_min_explicit, wear_max_explicit; };',
             'inline constexpr Item items[] = {']
    offset = 0
    for item in items:
        count = len(paint_lists[item['id']])
        lines.append('    {%d, %s, %s, Category::%s, %d, %d, %s},' %
                     (item['id'], cpp_string(item['name']), cpp_string(item['model']), item['category'],
                      offset, count, cpp_string(item['canonical'])))
        offset += count
    lines += ['};', 'inline constexpr Paint paints[] = {']
    for paint in paints:
        flags = ', '.join('true' if paint[key] else 'false' for key in
                          ('legacy_model', 'legacy_model_specified', 'wear_min_explicit', 'wear_max_explicit'))
        lines.append('    {%d, %s, %s, %s, %s, %s},' %
                     (paint['id'], cpp_string(paint['name']), cpp_float(paint['wear_min']),
                      cpp_float(paint['wear_max']), cpp_string(paint['internal']), flags))
    lines += ['};', 'inline constexpr int item_paints[] = {']
    for item in items:
        lines.append('    // ' + item['name'])
        values = paint_lists[item['id']]
        for start in range(0, len(values), 16):
            lines.append('    ' + ', '.join(str(value) for value in values[start:start+16]) + ',')
    lines += ['};',
              'constexpr const Item* find_item(int id) {',
              '    for (const auto& item : items) if (item.id == id) return &item;',
              '    return nullptr;', '}',
              'constexpr const Paint* find_paint(int id) {',
              '    std::size_t low = 0, high = sizeof(paints) / sizeof(paints[0]);',
              '    while (low < high) { const auto mid = low + (high - low) / 2;',
              '        if (paints[mid].id < id) low = mid + 1; else high = mid; }',
              '    return low < sizeof(paints) / sizeof(paints[0]) && paints[low].id == id ? &paints[low] : nullptr;', '}',
              'constexpr std::span<const int> paints_for(const Item& item) {',
              '    return {item_paints + item.paint_first, item.paint_count};', '}',
              'constexpr bool compatible(int item_id, int paint_id) {',
              '    const auto* item = find_item(item_id); if (!item) return false;',
              '    for (auto id : paints_for(*item)) if (id == paint_id) return true;',
              '    return false;', '}', '}', '']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vpk', type=Path, required=True)
    args = parser.parse_args()
    vpk = args.vpk.resolve()
    raw = assets(vpk, {ITEMS_ASSET, LANG_ASSET})
    schema = keyvalues(raw[ITEMS_ASSET])['items_game']
    tokens = {key.lower(): value for key, value in keyvalues(raw[LANG_ASSET])['lang']['Tokens'].items()}
    items, paints, pairs, coverage = build_catalog(schema, tokens, inventory(vpk))
    header = emit_header(items, paints, pairs)
    (ROOT / 'src/generated/cosmetics.hpp').write_text(header, encoding='utf-8')
    report = dict(vpk=str(vpk), vpk_directory_sha256=hashlib.sha256(vpk.read_bytes()).hexdigest(),
                  asset_sha256={key: hashlib.sha256(value).hexdigest() for key, value in sorted(raw.items())},
                  counts=dict(items=len(items), item_categories=dict(Counter(item['category'] for item in items)),
                              paint_kits=len(paints), compatible_pairs=len(pairs)), coverage=coverage,
                  semantics=dict(prefabs='Recursive named prefabs merged in listed order; item overrides last.',
                                 duplicate_kv_objects='Repeated objects recursively merge; scalar keys use last value.',
                                 compatibility='Exact inventory icon item/paint names, plus exact item-set/client-loot references; paint0 for every item.',
                                 legacy_model='Explicit use_legacy_model field only. Missing => value false and legacy_model_specified false; runtime default unverified.',
                                 wear='Missing remaps filled from paint0 min0.06/max0.8 with explicitness flags; engine default-copy behavior unverified.',
                                 scope='All shipped weapon_ item definitions and clothing_hands definitions. Inventory entries establish supported catalog pairings, not successful runtime rendering.'),
                  limitations=['Server-only unusual loot tables are absent; knife/glove pairings use installed inventory icon associations.',
                               'Archived assets cannot establish future updates, ownership, inventory grants, or runtime skin application.'],
                  items=items, paints=paints, compatible_pairs=pairs)
    (ROOT / 'analysis/cosmetics-source.json').write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(json.dumps(dict(counts=report['counts'], coverage={k:len(v) if isinstance(v,list) else v for k,v in coverage.items()})))


if __name__ == '__main__':
    main()
