"""Read-only cross-check of the inspected local server authority layouts."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
import struct

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pid',type=int,required=True)
    p.add_argument('--engine',type=int,required=True)
    p.add_argument('--network',type=int,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args(); k=c.WinDLL('kernel32',use_last_error=True)
    k.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD]; k.OpenProcess.restype=w.HANDLE
    k.ReadProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
    k.CloseHandle.argtypes=[w.HANDLE]
    handle=k.OpenProcess(0x1010,False,a.pid)
    if not handle: raise OSError(c.get_last_error(),'OpenProcess')
    def raw(address,size):
        b=c.create_string_buffer(size); n=c.c_size_t()
        return b.raw if address and k.ReadProcessMemory(handle,address,b,size,c.byref(n)) and n.value==size else None
    def value(address,fmt='<Q'):
        b=raw(address,struct.calcsize(fmt)); return struct.unpack(fmt,b)[0] if b else None
    def field(address,offset,fmt='<Q'):
        return value(address+offset,fmt) if address else None
    def table(address,base):
        v=field(address,0); return hex(v-base) if v else None
    try:
        service=value(a.engine+0x696a68); server=field(service,0x150)
        client=value(a.engine+0x91afc0); channel=field(client,0xf0)
        cheats=value(a.engine+0x69bf80); name=field(cheats,0)
        name_bytes=raw(name,10)
        result={'method':'ReadProcessMemory only; no game calls or writes',
            'server_service_vtable':table(service,a.engine),'server_vtable':table(server,a.engine),
            'server_state':field(server,0x38,'<i'),'max_clients':field(server,0x2bc,'<i'),
            'client_vtable':table(client,a.engine),'signon':field(client,0x230,'<i'),
            'background':field(client,0x2c143f,'<B'),'channel_vtable':table(channel,a.network),
            'channel_kind':field(channel,0x8eb0,'<i'),
            'convar_name':name_bytes.rstrip(b'\0').decode(errors='replace') if name_bytes else None,
            'convar_type':field(cheats,0x28,'<H'),'sv_cheats':field(cheats,0x58,'<B')}
        a.output.write_text(json.dumps(result,indent=2)+'\n'); print(json.dumps(result,indent=2))
    finally: k.CloseHandle(handle)

if __name__=='__main__': main()
