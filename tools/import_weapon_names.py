"""Derive weapon display names from the installed game's VPK items and localization."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]

def assets(directory, wanted):
    data = directory.read_bytes()
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
        end = data.index(b'\0', cursor, limit)
        value = data[cursor:end].decode('utf-8')
        cursor = end + 1
        return value
    result = {}
    while (extension := string()):
        while (folder := string()):
            while (name := string()):
                crc, preload, archive, offset, length, terminator = struct.unpack_from('<IHHIIH', data, cursor)
                cursor += 18
                if terminator != 0xFFFF or cursor + preload > limit:
                    raise ValueError('Malformed VPK entry')
                prefix = data[cursor:cursor+preload]; cursor += preload
                path = ('' if folder == ' ' else folder + '/') + name + ('' if extension == ' ' else '.' + extension)
                if path not in wanted:
                    continue
                if length + preload > 16 * 1024 * 1024:
                    raise ValueError('Requested asset exceeds size limit')
                carrier = directory if archive == 0x7FFF else directory.with_name(directory.name[:-8] + f'_{archive:03d}.vpk')
                with carrier.open('rb') as source:
                    source.seek(offset + (limit if archive == 0x7FFF else 0))
                    content = prefix + source.read(length)
                if len(content) != length + preload or zlib.crc32(content) != crc:
                    raise ValueError('VPK length/CRC mismatch: ' + path)
                result[path] = content
    if set(result) != set(wanted):
        raise ValueError('Missing assets: ' + str(set(wanted) - set(result)))
    return result

def keyvalues(raw):
    text = raw.decode('utf-16') if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else raw.decode('utf-8-sig')
    tokens = []
    for match in re.finditer(r'//[^\n]*|"((?:\\.|[^"\\])*)"|([{}])', text):
        if match.group(1) is not None:
            tokens.append(re.sub(r'\\(["\\])', r'\1', match.group(1)))
        elif match.group(2):
            tokens.append(match.group(2))
    index = 0
    def merge(previous, value):
        for key, item in value.items():
            if isinstance(previous.get(key), dict) and isinstance(item, dict):
                merge(previous[key], item)
            else:
                previous[key] = item
        return previous
    def parse(nested=False):
        nonlocal index
        result = {}
        while index < len(tokens):
            key = tokens[index]; index += 1
            if key == '}':
                if not nested: raise ValueError('Unbalanced KV object')
                return result
            if index >= len(tokens): raise ValueError('Missing KV value')
            value = tokens[index]; index += 1
            value = parse(True) if value == '{' else value
            result[key] = merge(result[key], value) if isinstance(result.get(key), dict) and isinstance(value, dict) else value
        if nested: raise ValueError('Unterminated KV object')
        return result
    return parse()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vpk', required=True, type=Path)
    args = parser.parse_args()
    raw = assets(args.vpk, {'scripts/items/items_game.txt', 'resource/csgo_english.txt'})
    items = keyvalues(raw['scripts/items/items_game.txt'])['items_game']
    tokens = {key.lower(): value for key, value in keyvalues(raw['resource/csgo_english.txt'])['lang']['Tokens'].items()}
    prefabs = items.get('prefabs', {})
    def token(item, depth=0):
        if 'item_name' in item: return item['item_name']
        if depth < 12:
            for name in item.get('prefab', '').split():
                found = token(prefabs.get(name, {}), depth+1)
                if found: return found
        return ''
    names = {}
    for key, item in items['items'].items():
        if not key.isdigit() or int(key) > 65535 or not isinstance(item, dict): continue
        canonical = item.get('name', '')
        if not canonical.startswith('weapon_'): continue
        display = tokens.get(token(item).lstrip('#').lower())
        names[int(key)] = display or canonical[7:].replace('_', ' ').upper()
    lines = ['#pragma once', '#include <cstdint>', '// Generated from local VPK assets; see analysis/weapon-names-source.json.', 'namespace weapon_names {', 'inline const char* lookup(std::uint16_t id) {', '    switch (id) {']
    lines += [f'    case {key}: return {json.dumps(value)};' for key, value in sorted(names.items())]
    lines += ['    default: return nullptr;', '    }', '}', '}', '']
    (ROOT/'src/generated/weapon_names.hpp').write_text('\n'.join(lines), encoding='utf-8')
    (ROOT/'analysis/weapon-names-source.json').write_text(json.dumps({'vpk':str(args.vpk), 'asset_sha256':{key:hashlib.sha256(value).hexdigest() for key,value in raw.items()}, 'names':names}, indent=2)+'\n')
    print('Generated', len(names), 'weapon labels. ID 61:', names.get(61))

if __name__ == '__main__':
    main()
