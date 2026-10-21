"""Generate exact-build guards for locally recovered native input/target methods."""
import hashlib
import json
from pathlib import Path
from inspect_pe import PE

ROOT = Path(__file__).resolve().parents[1]
SHA256 = '5a46a09b521bbd607012146364dafae2abfa50364246638391990d4c7dfb9d49'
ENGINE_SHA256 = '6456df18d8f954394024c2c07da8c848df15a5ace74ce7390d78970ceb39ee21'
FUNCTIONS = {'input_frame':0xb65de0, 'get_angles':0xb6f680, 'set_angles':0xb82210,
             'bone_index':0x952940, 'bone_position':0x93f370, 'trace_line':0x826be0,
             'get_spread':0x80d490, 'get_inaccuracy':0x80c1a0, 'get_punch':0x8cca20,
             'filter_construct':0x352920, 'trace_construct':0x87ee00, 'trace_destruct':0x87fb70,
             'bullet_create':0x8830e0, 'bullet_penetrate':0x885710, 'bullet_interval':0x88b450,
             'hit_group':0xaadf40, 'is_armored':0x8d8cf0, 'through_smoke':0xc8a7d0,
             'get_hitboxes':0xaade10, 'get_skeleton':0x1c9e70,
             'primary_tick_ready':0x94de60, 'history_parse':0xcfa6b0}

def main():
    profile=json.loads((ROOT/'analysis/dump-profile.json').read_text())
    image=PE(profile['images']['client']['path'])
    digest=hashlib.sha256(image.data).hexdigest()
    if digest != SHA256 or profile['info']['build_number'] != 14191:
        raise SystemExit('Aim native calls require a newly inspected binary profile.')
    lines=['#pragma once','#include <array>','#include <cstdint>',
           '// Local LLVM static recovery; see analysis/aim-*-recovery.json.', 'namespace aim_native {']
    methods={}
    for name,rva in FUNCTIONS.items():
        guard=image.data[image.offset(rva):image.offset(rva)+32]
        methods[name]={'rva':hex(rva),'first_32_bytes':guard.hex()}
        lines.extend([f'inline constexpr std::uint32_t {name} = 0x{rva:x};',
                      f'inline constexpr std::array<unsigned char,32> {name}_bytes{{'+','.join(f'0x{x:02x}' for x in guard)+'};'])
    engine=PE(profile['images']['engine']['path'])
    if hashlib.sha256(engine.data).hexdigest() != ENGINE_SHA256:
        raise SystemExit('Native action calls require the inspected engine binary.')
    guard=engine.data[engine.offset(0x190b80):engine.offset(0x190b80)+32]
    lines.extend(['inline constexpr std::uint32_t action_delta = 0x190b80;',
                  'inline constexpr std::array<unsigned char,32> action_delta_bytes{'+','.join(f'0x{x:02x}' for x in guard)+'};',
                  'inline constexpr std::array<std::uint32_t,6> actions{0x2233ea8,0x2233f38,0x22340e8,0x2234178,0x2234208,0x2234298};'])
    lines.extend(['inline constexpr std::uint32_t input_vtable = 0x1c9db20;',
                  'inline constexpr std::uint32_t trace_query = 0x2227058;',
                  'inline constexpr std::uint32_t teammates_are_enemies = 0x24bcf90;',
                  'inline constexpr std::uint32_t global_vars = 0x2230090, game_rules = 0x25651d0;',
                  'inline constexpr std::uint32_t game_rules_vtable = 0x1bb3c50, game_rules_tick = 0xba7850;',
                  'inline constexpr unsigned input_slot = 5;', '}', ''])
    (ROOT/'src/generated/aim_native.hpp').write_text('\n'.join(lines))
    (ROOT/'analysis/aim-native-profile.json').write_text(json.dumps({
        'build':14191,'sha256':digest,'methods':methods,
        'source':'Local PE and LLVM disassembly; not REA decompilation',
        'compatibility_evidence':'analysis/esp-client-compatibility-recovery.json; complete entry bodies, used helpers and RTTI tables compared with the prior inspected binary',
        'bone_contract':{'index':'int(entity*, const char* name); scene virtual slot13; model/hash lookup',
                         'position':'void(entity*, int index, Vec3* worldPosition, Vec3* worldAngles)',
                         'position_evidence':['0x93f370 validates index against model count',
                                              '0x93f416 retrieves world bone transform via 0xab11b0',
                                              '0xd24f80 passes index from 0x952940 and reads world XYZ output']},
        'recoil_contract':{'prototype':'Vec3* getPunch(pawn*, Vec3* output, bool predictionMode)',
                           'getter':'0x8cca20 loads pawn+1598, delegates to0x8526b0 preserving R8b',
                           'composition':'0x8526b0 evaluates predictable and unpredictable angles through0x84d8b0 and adds via0x84b2c0',
                           'shooting_evidence':'0x904f56 passes booltrue to getter;0x904f6b adds returned punch to pawn virtual570 eye angles',
                           'independent_caller':'0xd239df getter booltrue,0xd239ee same vector-add; no guessed recoil multiplier'}
    },indent=2)+'\n')
    print('Generated exact-build aim native profile')

if __name__ == '__main__': main()
