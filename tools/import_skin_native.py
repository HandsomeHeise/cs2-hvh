"""Bind recovered cosmetic entry points to the exact locally inspected client binary."""
import hashlib
import json
from pathlib import Path
from inspect_pe import PE

ROOT = Path(__file__).resolve().parents[1]
# These are recovered entry points, not signatures to guess on a different build.
SHA256 = '5a46a09b521bbd607012146364dafae2abfa50364246638391990d4c7dfb9d49'
FUNCTIONS = {'frame':0xb6d700, 'set_attribute':0x11a0e50, 'invalidate_item':0x119c980,
             'weapon_model':0x118f870, 'set_model':0x9643c0, 'weapon_changed':0x821170,
             'clear_materials':0x14e7aa0, 'rebuild_materials':0x802950,
             'uses_legacy_model':0x825770, 'set_mesh_mask':0xac41b0,
             'body_group_index':0x93c960, 'set_body_choice':0xac15a0}

def main():
    profile = json.loads((ROOT/'analysis/dump-profile.json').read_text())
    image = PE(profile['images']['client']['path'])
    digest = hashlib.sha256(image.data).hexdigest()
    if digest != SHA256 or profile['info']['build_number'] != 14191:
        raise SystemExit('Cosmetic native calls require a newly inspected binary profile.')
    lines=['#pragma once','#include <cstdint>','#include <array>',
           '// tools/import_skin_native.py; static disassembly in analysis/cosmetic-*-recovery.json.',
           'namespace skin_native {',f'inline constexpr char client_sha256[] = "{digest}";']
    methods={}
    for name,rva in FUNCTIONS.items():
        code=image.data[image.offset(rva):image.offset(rva)+32]
        lines += [f'inline constexpr std::uint32_t {name} = 0x{rva:x};',
                  f'inline constexpr std::array<unsigned char, 32> {name}_bytes{{'+','.join(f'0x{x:02x}' for x in code)+'};']
        methods[name]={'rva':hex(rva),'first_32_bytes':code.hex()}
    lines+=['inline constexpr std::uint32_t client_interface = 0x255f3e0;',
            'inline constexpr std::uint32_t client_vtable = 0x1c630f0;',
            'inline constexpr std::uint32_t skeleton_vtable = 0x1c41ce0;',
            'inline constexpr std::uint32_t networked_skeleton_vtable = 0x1acde48;',
            'inline constexpr std::uint32_t skeleton_get = 0x1c9e60;',
            'inline constexpr std::uint32_t hud_weapon_vtable = 0x1d09de0, hud_arms_vtable = 0x1d0a760;',
            'inline constexpr unsigned frame_slot = 36, weapon_changed_slot = 11;',
            'inline constexpr unsigned attribute_stride = 0x48;', '}', '']
    (ROOT/'src/generated/skin_native.hpp').write_text('\n'.join(lines))
    (ROOT/'analysis/skin-native-profile.json').write_text(json.dumps({
        'client_sha256':digest,'build':14191,'methods':methods,
        'source':'Local PE, pdata unwind ranges and LLVM disassembly; not REA decompilation evidence',
        'compatibility_evidence':'analysis/esp-cosmetic-compatibility.json; bounded whole-body and RTTI comparison with the prior inspected binary',
        'frame_timing':'Stage 6 emits FramePostDataUpdate; hook observes game thread. Visual behavior requires live validation.'},indent=2)+'\n')
    print('Generated exact-build cosmetic native profile')

if __name__ == '__main__': main()
