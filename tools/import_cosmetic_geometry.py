"""Read exact cosmetic body choices from CRC-checked local model resources.

The bounded KV3 v5 decoder follows the public ValveResourceFormat binary layout.
It supports uncompressed/LZ4 DATA and CTRL blocks without external packages.
Unknown formats and ambiguous choices remain unsupported rather than receiving
a guessed mesh mask. No game process is opened and no game asset is modified.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

from import_weapon_names import assets

ROOT = Path(__file__).resolve().parents[1]
FORMAT_COMMIT = '0d72f09ac9f36cbc4eae04b8541defeba1802ff5'
FORMAT_SOURCE = 'https://github.com/ValveResourceFormat/ValveResourceFormat/blob/' + FORMAT_COMMIT + '/ValveResourceFormat/Resource/ResourceTypes/'


def lz4(source, expected):
    """Decode one bounded raw LZ4 block, including overlapping match copies."""
    if not 0 <= expected <= 4 * 1024 * 1024:
        raise ValueError('KV3 decompressed size exceeds bound')
    output = bytearray()
    cursor = 0

    def length(initial):
        nonlocal cursor
        result = initial
        if result == 15:
            while True:
                if cursor >= len(source):
                    raise ValueError('Truncated LZ4 length')
                extra = source[cursor]
                cursor += 1
                result += extra
                if extra != 255:
                    break
        return result

    while cursor < len(source):
        token = source[cursor]
        cursor += 1
        count = length(token >> 4)
        if cursor + count > len(source) or len(output) + count > expected:
            raise ValueError('Invalid LZ4 literals')
        output.extend(source[cursor:cursor + count])
        cursor += count
        if cursor == len(source):
            break
        if cursor + 2 > len(source):
            raise ValueError('Truncated LZ4 offset')
        offset = int.from_bytes(source[cursor:cursor + 2], 'little')
        cursor += 2
        count = length(token & 15) + 4
        if not 0 < offset <= len(output) or len(output) + count > expected:
            raise ValueError('Invalid LZ4 match')
        for _ in range(count):
            output.append(output[-offset])
    if len(output) != expected:
        raise ValueError('LZ4 decoded size mismatch')
    return bytes(output)


class Lane:
    def __init__(self, content):
        self.content, self.cursor = content, 0

    def take(self, count):
        if count < 0 or self.cursor + count > len(self.content):
            raise ValueError('Truncated KV3 lane')
        result = self.content[self.cursor:self.cursor + count]
        self.cursor += count
        return result

    def get(self, fmt):
        return struct.unpack('<' + fmt, self.take(struct.calcsize('<' + fmt)))[0]

    def string(self):
        end = self.content.index(b'\0', self.cursor)
        result = self.content[self.cursor:end].decode('utf-8')
        self.cursor = end + 1
        return result

    def remaining(self):
        return len(self.content) - self.cursor


def split_lanes(content, counts, offset=0):
    result = {}
    for width, count in zip((1, 2, 4, 8), counts):
        if not 0 <= count <= 1000000:
            raise ValueError('KV3 lane count exceeds bound')
        if count:
            offset = (offset + width - 1) & ~(width - 1)
        end = offset + width * count
        if end > len(content):
            raise ValueError('Truncated KV3 lane allocation')
        result[width] = Lane(content[offset:end])
        offset = end
    return result, offset


def decode_kv3(content):
    if len(content) < 120 or struct.unpack_from('<I', content)[0] != 0x4b563305:
        raise ValueError('Only binary KV3 v5 is supported')
    comp, dictionary, frame, c1, c4, c8, types_count, _, _, total, compressed, blobs, blob_size, c2, block_sizes = struct.unpack_from('<IHHIIIIHHIIIIII', content, 20)
    u1, z1, u2, z2, x1, x2, x4, x8, _, objects, _, _ = struct.unpack_from('<12I', content, 72)
    if dictionary or blobs or blob_size or block_sizes or total != u1 + u2 or max(u1, u2) > 4 * 1024 * 1024:
        raise ValueError('Unsupported KV3 dictionary/blob or invalid buffer size')
    if comp == 1 and frame == 0x4000:
        if 120 + z1 + z2 != len(content) or compressed != z1 + z2:
            raise ValueError('KV3 compressed size mismatch')
        first = lz4(content[120:120 + z1], u1)
        second = lz4(content[120 + z1:120 + z1 + z2], u2)
    elif comp == 0 and frame == 0 and not z1 and not z2:
        if 120 + u1 + u2 != len(content):
            raise ValueError('KV3 uncompressed size mismatch')
        first, second = content[120:120 + u1], content[120 + u1:]
    else:
        raise ValueError('Unsupported KV3 compression')
    auxiliary, first_end = split_lanes(first, (c1, c2, c4, c8))
    if first_end != len(first):
        raise ValueError('KV3 auxiliary trailing data')
    strings_count = auxiliary[4].get('i')
    if not 0 <= strings_count <= 100000:
        raise ValueError('KV3 string count exceeds bound')
    strings = [auxiliary[1].string() for _ in range(strings_count)]
    lengths = Lane(second[:objects * 4])
    main, second_end = split_lanes(second, (x1, x2, x4, x8), objects * 4)
    types = Lane(second[second_end:second_end + types_count])
    if second_end + types_count + 4 != len(second) or struct.unpack_from('<I', second, second_end + types_count)[0] != 0xffeedd00:
        raise ValueError('KV3 trailer mismatch')

    def node_type():
        result = types.get('B')
        if result & 0x80:
            types.get('B')  # Value flag, not a different primitive storage lane.
        if result & 0x40:
            types.get('B')
        return result & 0x3f

    def string_value():
        index = main[4].get('i')
        return strings[index] if 0 <= index < len(strings) else ''

    def count(value):
        if not 0 <= value <= 1000000:
            raise ValueError('KV3 container count exceeds bound')
        return value

    def value(kind, lane, depth=0):
        if depth > 64:
            raise ValueError('KV3 nesting exceeds bound')
        constants = {1: None, 13: True, 14: False, 15: 0, 16: 1, 17: 0., 18: 1., 26: 0, 27: 1}
        if kind in constants:
            return constants[kind]
        scalar = {2: (1, 'B'), 3: (8, 'q'), 4: (8, 'Q'), 5: (8, 'd'), 11: (4, 'i'), 12: (4, 'I'), 19: (4, 'f'), 20: (2, 'h'), 21: (2, 'H'), 22: (1, 'b'), 23: (1, 'B')}
        if kind in scalar:
            width, fmt = scalar[kind]
            result = lane[width].get(fmt)
            return bool(result) if kind == 2 else result
        if kind == 6:
            return string_value()
        if kind == 9:
            result = {}
            for _ in range(count(lengths.get('i'))):
                subtype = node_type()
                name = string_value()
                result[name] = value(subtype, main, depth + 1)
            return result
        if kind == 8:
            return [value(node_type(), main, depth + 1) for _ in range(count(main[4].get('i')))]
        if kind in (10, 24, 25):
            length = count(main[4].get('i') if kind == 10 else main[1].get('B'))
            subtype = node_type()
            return [value(subtype, auxiliary if kind == 25 else main, depth + 1) for _ in range(length)]
        raise ValueError('Unsupported KV3 node type ' + str(kind))

    result = value(node_type(), main)
    remaining = [types.remaining(), lengths.remaining(), *[lane.remaining() for lane in main.values()], *[lane.remaining() for lane in auxiliary.values()]]
    if any(remaining):
        raise ValueError('KV3 lanes not fully consumed')
    return result, {'version': 5, 'compression': 'LZ4' if comp == 1 else 'uncompressed', 'all_lanes_consumed': True, 'source_size': len(content), 'buffer1_size': u1, 'buffer2_size': u2, 'string_count': strings_count}


def resource_blocks(content):
    size, version, _, offset, count = struct.unpack_from('<IHHII', content)
    if size != len(content) or version != 12 or count > 128:
        raise ValueError('Unsupported resource header')
    result = []
    for index in range(count):
        entry = 8 + offset + index * 12
        kind, relative, length = struct.unpack_from('<4sII', content, entry)
        start = entry + 4 + relative
        if start + length > len(content):
            raise ValueError('Resource block exceeds file size')
        result.append({'index': index, 'kind': kind.decode('ascii'), 'offset': start, 'length': length, 'content': content[start:start + length]})
    return result


def inspect_model(content):
    blocks = resource_blocks(content)
    selected = {}
    for kind in ('DATA', 'CTRL'):
        block = next((b for b in blocks if b['kind'] == kind), None)
        if block:
            data, metadata = decode_kv3(block['content'])
            selected[kind] = {'index': block['index'], 'offset': block['offset'], 'size': block['length'], 'sha256': hashlib.sha256(block['content']).hexdigest(), 'decoder': metadata, 'data': data}
    return selected


def geometry_record(item, content):
    record = dict(item)
    record.update(resource=item['model'] + '_c', sha256=hashlib.sha256(content).hexdigest(), size=len(content), eligible=False,
                  modern_mask=0, legacy_mask=0, switch_mask=0, default_mask=0,
                  modern_choice=0xffffffff, legacy_choice=0xffffffff)
    try:
        selected = inspect_model(content)
        model = selected['DATA']['data']
        fields = ['m_meshGroups', 'm_refMeshGroupMasks', 'm_nDefaultMeshGroupMask', 'm_refMeshes', 'm_refLODGroupMasks', 'm_BodyGroupsHiddenInTools']
        record['model_data'] = {key: model.get(key) for key in fields}
        record['block_evidence'] = {kind: {k: v for k, v in block.items() if k != 'data'} for kind, block in selected.items()}
        record['default_mask'] = model.get('m_nDefaultMeshGroupMask', 0)
        groups = model.get('m_meshGroups', [])
        masks = model.get('m_refMeshGroupMasks', [])
        embedded = selected.get('CTRL', {}).get('data', {}).get('embedded_meshes', [])
        blocks = resource_blocks(content)
        choices = {}
        for index, name in enumerate(groups):
            match = re.fullmatch(r'body_@(\d+)_#&(body_hd|body_legacy|body_hidden)', name)
            if match:
                choices.setdefault(match[2], []).append((index, int(match[1])))
        record['embedded_meshes'] = []
        for mesh in embedded:
            index = mesh['m_nMeshIndex']
            evidence = {key: mesh[key] for key in ('m_Name', 'm_nMeshIndex', 'm_nDataBlock')}
            evidence['group_mask'] = masks[index] if 0 <= index < len(masks) else None
            try:
                block = blocks[mesh['m_nDataBlock']]
                if block['kind'] != 'MDAT':
                    raise ValueError('Embedded data block is not MDAT')
                data, metadata = decode_kv3(block['content'])
                evidence['data_block_sha256'] = hashlib.sha256(block['content']).hexdigest()
                evidence['decoder'] = metadata
                evidence['draw_calls'] = [
                    {key: call.get(key) for key in ('m_material', 'm_nVertexCount', 'm_nIndexCount', 'm_flUvDensity')}
                    for scene in data.get('m_sceneObjects', []) for call in scene.get('m_drawCalls', [])
                ]
            except (ValueError, KeyError, IndexError, struct.error) as error:
                evidence['draw_call_evidence_unavailable'] = str(error)
            record['embedded_meshes'].append(evidence)
        if len(choices.get('body_hd', [])) != 1 or len(choices.get('body_legacy', [])) != 1:
            record['reason'] = 'No unique compiled body_hd/body_legacy pair'
            return record
        (hd, modern_choice), (legacy, legacy_choice) = choices['body_hd'][0], choices['body_legacy'][0]
        if max(modern_choice, legacy_choice) >= 0xffffffff or modern_choice == legacy_choice:
            record['reason'] = 'Invalid or repeated compiled body choice value'
            return record
        if max(hd, legacy) >= 64:
            record['reason'] = 'Choice index exceeds native 64-bit mesh mask'
            return record
        modern_mask, legacy_mask = 1 << hd, 1 << legacy
        for name, chosen, opposite in (('body_hd', modern_mask, legacy_mask), ('body_legacy', legacy_mask, modern_mask)):
            corresponding = [mesh for mesh in record['embedded_meshes'] if mesh['m_Name'] == name]
            if len(corresponding) != 1 or corresponding[0]['group_mask'] is None or not corresponding[0]['group_mask'] & chosen or corresponding[0]['group_mask'] & opposite:
                record['reason'] = 'Embedded mesh membership does not corroborate choice ' + name
                return record
        record.update(eligible=True, modern_mask=modern_mask, legacy_mask=legacy_mask, switch_mask=modern_mask | legacy_mask,
                      modern_choice=modern_choice, legacy_choice=legacy_choice,
                      modern_default_mask=(record['default_mask'] & ~(modern_mask | legacy_mask)) | modern_mask,
                      legacy_default_mask=(record['default_mask'] & ~(modern_mask | legacy_mask)) | legacy_mask,
                      reason='Unique named body choices corroborated by embedded mesh membership')
    except (ValueError, KeyError, IndexError, struct.error) as error:
        record['reason'] = str(error)
        record['format_unsupported'] = True
    return record


def generate_header(records):
    lines = ['#pragma once', '#include <cstdint>',
             '// Generated by tools/import_cosmetic_geometry.py from CRC-checked installed model resources.',
             '// Choice masks are group bits only; select_mask preserves unrelated model groups.',
             '// See analysis/cosmetic-legacy-assets.json for resource hashes, typed fields and coverage.',
             'namespace cosmetic_geometry {',
             'struct Model { std::uint16_t id; std::uint64_t modern_mask, legacy_mask, switch_mask, default_mask; std::uint32_t modern_choice, legacy_choice; bool eligible; const char* sha256; };',
             'inline constexpr Model models[] = {']
    for record in records:
        masks = ', '.join('0x{:x}ULL'.format(record[key]) for key in ('modern_mask', 'legacy_mask', 'switch_mask', 'default_mask'))
        choices = ', '.join('0x{:x}U'.format(record[key]) for key in ('modern_choice', 'legacy_choice'))
        lines.append('    {' + f'{record["id"]}, {masks}, {choices}, {str(record["eligible"]).lower()}, "{record["sha256"]}"' + '},')
    lines += ['};',
              'constexpr const Model* find(int id) {',
              '    for (const auto& model : models) if (model.id == id) return &model;',
              '    return nullptr;', '}',
              'constexpr std::uint64_t select_mask(const Model& model, std::uint64_t current, bool legacy) {',
              '    // Leave a hidden/unselected body hidden rather than forcing a visible body.',
              '    if (!model.eligible || !(current & model.switch_mask)) return current;',
              '    return (current & ~model.switch_mask) | (legacy ? model.legacy_mask : model.modern_mask);',
              '}', '}', '']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vpk', type=Path, required=True)
    parser.add_argument('--catalog', type=Path, default=ROOT / 'analysis/cosmetics-source.json')
    parser.add_argument('--header', type=Path, default=ROOT / 'src/generated/cosmetic_geometry.hpp')
    parser.add_argument('--report', type=Path, default=ROOT / 'analysis/cosmetic-legacy-assets.json')
    args = parser.parse_args()
    catalog = json.loads(args.catalog.read_text(encoding='utf-8'))
    items = [item for item in catalog['items'] if item['category'] == 'weapon']
    wanted = list(dict.fromkeys(item['model'] + '_c' for item in items))
    content = assets(args.vpk, wanted)  # Exact length and CRC validation for every model.
    records = [geometry_record(item, content[item['model'] + '_c']) for item in items]
    records.sort(key=lambda record: record['id'])
    report = {
        'method': 'Bounded offline VPK extraction and typed binary KV3 decoding; no game execution or memory changes.',
        'vpk': str(args.vpk.resolve()), 'vpk_directory_sha256': hashlib.sha256(args.vpk.read_bytes()).hexdigest(),
        'catalog_sha256': hashlib.sha256(args.catalog.read_bytes()).hexdigest(),
        'decoder': {'implementation': 'tools/import_cosmetic_geometry.py; standard library only',
                    'supported': 'Resource v12; binary KV3 v5; independent uncompressed/LZ4 buffers; no binary blobs',
                    'format_references': [FORMAT_SOURCE + path for path in ('BinaryKV3.cs', 'BinaryKV3.NodeType.cs', 'ModelData/ModelMeshGroups.cs')],
                    'upstream_commit': FORMAT_COMMIT,
                    'validation': 'VPK CRC, declared resource/block sizes, LZ4 output sizes, KV3 trailer and exhaustive consumption of all value/type/object lanes'},
        'counts': {'weapon_definitions': len(records), 'models_decoded': sum('model_data' in r for r in records),
                   'eligible_legacy_body_pairs': sum(r['eligible'] for r in records),
                   'no_pair_or_unsupported': sum(not r['eligible'] for r in records),
                   'unsupported_formats': sum(r.get('format_unsupported', False) for r in records)},
        'semantics': {'group_index': 'Bit N uses the declaration position N in m_meshGroups, not an assumed name suffix number.',
                      'modern_choice_legacy_choice': 'Actual decimal N captured from body_@N_#&body_hd/body_legacy; independent of declaration positions and mesh-mask bits. Ineligible uses 0xffffffff.',
                      'modern_mask_legacy_mask': 'Bits for the paired named body_hd/body_legacy choices, not a whole-model mask.',
                      'switch_mask': 'Only the paired body bits. Other groups, e.g. Dual Berettas autodefault bit8, are preserved.',
                      'hidden_body': 'select_mask leaves current unchanged if neither paired body bit is active.',
                      'default_mask': 'Full m_nDefaultMeshGroupMask read from the asset; not guessed from paint ID.'},
        'models': records,
        'limitations': ['Asset membership proves the model choices and their masks; native setter identity, render scheduling, and successful live UV mapping require independent evidence.',
                       'Models without a unique named pair are ineligible; no global1/2 fallback is generated.',
                       'Only catalogue category weapon main models are examined. Other attachments or model classes need their own resource mapping.',
                       'The decoder deliberately rejects unsupported resource/KV3 variants rather than partially parsing them.'],
    }
    args.header.parent.mkdir(parents=True, exist_ok=True)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.header.write_text(generate_header(records), encoding='utf-8')
    args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report['counts']))


if __name__ == '__main__':
    main()
