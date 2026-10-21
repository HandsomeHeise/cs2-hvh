"""Read-only sample of the exact-build replicated FFA rule (no game calls)."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
import struct


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid',type=int,required=True)
    parser.add_argument('--client',type=int,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    kernel=c.WinDLL('kernel32',use_last_error=True)
    kernel.OpenProcess.argtypes=[w.DWORD,w.BOOL,w.DWORD]; kernel.OpenProcess.restype=w.HANDLE
    kernel.ReadProcessMemory.argtypes=[w.HANDLE,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
    kernel.CloseHandle.argtypes=[w.HANDLE]
    handle=kernel.OpenProcess(0x1010,False,args.pid)
    if not handle: raise OSError(c.get_last_error(),'OpenProcess')
    def raw(address,size):
        output=c.create_string_buffer(size); count=c.c_size_t()
        if not address or not kernel.ReadProcessMemory(handle,address,output,size,c.byref(count)) or count.value!=size:
            raise OSError(c.get_last_error(),'ReadProcessMemory')
        return output.raw
    def value(address,fmt='<Q'):
        return struct.unpack(fmt,raw(address,struct.calcsize(fmt)))[0]
    try:
        pe=args.client+value(args.client+0x3c,'<I')
        if raw(pe,4)!=b'PE\0\0' or value(pe+8,'<I')!=0x6ad7b41c or value(pe+0x50,'<I')!=0x299d000:
            raise RuntimeError('Client metadata differs from the inspected profile')
        data=value(args.client+0x24bcf90); name=value(data)
        text=raw(name,25).split(b'\0',1)[0].decode('ascii')
        kind=value(data+0x28,'<H'); enabled=value(data+0x58,'<B')
        if text!='mp_teammates_are_enemies' or kind!=0 or enabled not in (0,1) or value(args.client+0x24bcf90)!=data:
            raise RuntimeError('Canonical rule identity/type/value validation failed')
        result={'method':'ReadProcessMemory only; no game calls or writes','pid':args.pid,
                'rule':text,'type':kind,'value':enabled,'enemy_rules':'Free-for-All' if enabled else 'Team-Based'}
        args.output.write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps(result))
    finally:
        kernel.CloseHandle(handle)


if __name__=='__main__': main()
