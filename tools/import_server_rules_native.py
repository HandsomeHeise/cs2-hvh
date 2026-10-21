"""Bind paired local-server weapon filters to the exact inspected binaries."""
from pathlib import Path
import hashlib
import json
from inspect_pe import PE

ROOT=Path(__file__).resolve().parents[1]
GAME=Path(r'C:/Program Files (x86)/Steam/steamapps/common/Counter-Strike Global Offensive/game')
IMAGES={
    'client':('csgo/bin/win64/client.dll','5a46a09b521bbd607012146364dafae2abfa50364246638391990d4c7dfb9d49'),
    'server':('csgo/bin/win64/server.dll','05e6f04972bb2983f463c08824c3133626e0703e881443589288b68e200acc4d'),
    'engine':('bin/win64/engine2.dll','6456df18d8f954394024c2c07da8c848df15a5ace74ce7390d78970ceb39ee21'),
    'network':('bin/win64/networksystem.dll','be7e13ad771b21c986e25d9469993a1225be5649473f3d722ca5eefe375d1bec')}
FUNCTIONS={'client_spread':('client',0x80d490),'client_inaccuracy':('client',0x80c1a0),'client_recoil':('client',0x85dc60),
           'server_spread':('server',0x9faba0),'server_inaccuracy':('server',0x9f9620),'server_recoil':('server',0xa446f0),
           'server_active':('engine',0x78b30),'loopback':('network',0xbb540)}

def main():
    lines=['#pragma once','#include <cstdint>','#include <array>',
           '// Exact-build local static evidence: analysis/aim-server-rules-recovery.json and analysis/aim-console-recovery.json.', 'namespace server_rules_native {']
    images={}; report={'images':{},'functions':{}}
    for name,(path,expected) in IMAGES.items():
        image=PE(GAME/path); digest=hashlib.sha256(image.data).hexdigest()
        if digest!=expected:raise SystemExit(f'{name} requires a newly inspected native profile')
        images[name]=image; pe=image.u32(0x3c)
        stamp=image.u32(pe+8); size=image.u32(pe+24+56)
        lines.extend([f'inline constexpr std::uint32_t {name}_timestamp=0x{stamp:x}, {name}_image_size=0x{size:x};'])
        report['images'][name]={'sha256':digest,'timestamp':hex(stamp),'image_size':hex(size)}
    for name,(module,rva) in FUNCTIONS.items():
        image=images[module]; b=image.data[image.offset(rva):image.offset(rva)+32]
        lines.extend([f'inline constexpr std::uint32_t {name}=0x{rva:x};',
                      f'inline constexpr std::array<unsigned char,32> {name}_bytes{{'+','.join(f'0x{x:02x}' for x in b)+'};'])
        report['functions'][name]={'module':module,'rva':hex(rva),'first32':b.hex()}
    lines.extend(['inline constexpr std::uint32_t server_service=0x696a68, server_service_vtable=0x5814c8;',
                  'inline constexpr std::uint32_t game_server_vtable=0x548df8, network_client_vtable=0x540bc8, channel_vtable=0x2358a0;',
                  'inline constexpr std::uint32_t cheats=0x69bf80;', '}', ''])
    (ROOT/'src/generated/server_rules_native.hpp').write_text('\n'.join(lines))
    (ROOT/'analysis/server-rules-native-profile.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Generated exact-build paired weapon filter profile')

if __name__=='__main__':main()
