"""Read-only native input/aim metadata probe; never calls or writes game functions."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pid',type=int,required=True)
    p.add_argument('--client',type=int,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    k=c.WinDLL('kernel32',use_last_error=True)
    k.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD]; k.OpenProcess.restype=w.HANDLE
    k.ReadProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
    k.CloseHandle.argtypes=[w.HANDLE]
    process=k.OpenProcess(0x1010,False,a.pid)
    if not process: raise OSError(c.get_last_error(),'OpenProcess')
    def raw(address,size):
        b=c.create_string_buffer(size); n=c.c_size_t()
        return b.raw if address and k.ReadProcessMemory(process,address,b,size,c.byref(n)) and n.value==size else None
    def value(address,fmt='<Q'):
        b=raw(address,struct.calcsize(fmt)); return struct.unpack(fmt,b)[0] if b else None
    def vector(address):
        b=raw(address,12); return list(struct.unpack('<3f',b)) if b else None
    def text(address):
        b=raw(address,128); return b.split(b'\0',1)[0].decode(errors='replace') if b else None
    o=json.loads((ROOT/'analysis/dump-profile.json').read_text())['values']; base=a.client
    pawn=value(base+o['local_pawn']); node=value(pawn+o['scene_node']) if pawn else 0
    identity=value(pawn+o['identity']) if pawn else 0
    service=value(pawn+o['aim_punch_services']) if pawn else 0
    input_service=value(base+0x279d0a0)
    input_table=value(base+o['csgo_input'])
    system=value(base+o['entity_system']); controllers=0; opponents=0; types={}
    local_team=value(pawn+o['team'],'<B') if pawn else None
    def indexed(index):
        page=value(system+0x10+8*(index//512)) if system else 0
        identity=page+(index%512)*0x70 if page else 0
        entity=value(identity) if identity else 0
        return entity if entity and value(entity+o['identity'])==identity else 0
    highest=value(system+0x2120,'<i') if system else 0
    for index in range(1,min(highest,8192)+1 if highest and highest>0 else 65):
        entity=indexed(index)
        identity=value(entity+o['identity']) if entity else 0
        designer=text(value(identity+o['designer_name'])) if identity else None
        if not designer: continue
        types[designer]=types.get(designer,0)+1
        if designer!='cs_player_controller': continue
        controllers+=1; handle=value(entity+o['pawn_handle'],'<I')
        target=indexed(handle&0x7fff) if handle is not None and handle!=0xffffffff else 0
        target_identity=value(target+o['identity']) if target else 0
        if target_identity and value(target_identity+0x10,'<I')==handle and value(target+o['health'],'<i') and value(target+o['team'],'<B')!=local_team:
            opponents+=1
    result={'method':'ReadProcessMemory only; no game calls, input or memory writes',
            'input_table_rva':hex(input_table-base) if input_table else None,
            'input_callback_rva':hex(value(input_table+40)-base) if input_table and value(input_table+40) else None,
            'pawn_designer':text(value(identity+o['designer_name'])) if identity else None,
            'origin':vector(node+o['abs_origin']) if node else None,
            'view_offset':vector(pawn+o['view_offset']) if pawn else None,
            'punch_owner_matches':value(service+0x38)==pawn if service else False,
            'trace_query_present':bool(value(base+0x2227058)),
            'highest_entity_index':highest,'entity_designer_types':types,'controllers':controllers,'opponents_alive':opponents,
            'attack_object_0x40':raw(base+0x2233ea8,0x40).hex(),
            'input_service_32f80_header':raw(input_service+0x32f80,0x30).hex() if input_service and raw(input_service+0x32f80,0x30) else None}
    k.CloseHandle(process)
    a.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))

if __name__=='__main__': main()
