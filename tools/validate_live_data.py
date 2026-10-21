"""Bounded read-only validation of selected dump fields in the user's CS2 process."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
import struct

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--client", type=int, required=True)
    parser.add_argument("--engine", type=int, required=True)
    args = parser.parse_args()
    kernel = c.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]; kernel.OpenProcess.restype = w.HANDLE
    kernel.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
    kernel.ReadProcessMemory.restype = w.BOOL
    kernel.CloseHandle.argtypes = [w.HANDLE]
    process = kernel.OpenProcess(0x10 | 0x1000, False, args.pid)
    if not process:
        raise OSError(c.get_last_error(), "OpenProcess")
    def read(address, count):
        buffer = c.create_string_buffer(count); copied = c.c_size_t()
        if not address or not kernel.ReadProcessMemory(process, address, buffer, count, c.byref(copied)) or copied.value != count:
            return None
        return buffer.raw
    def integer(address, fmt="<Q"):
        raw = read(address, struct.calcsize(fmt))
        return struct.unpack(fmt, raw)[0] if raw is not None else None
    def string(address):
        raw = read(address, 128)
        return raw.split(b"\0", 1)[0].decode("utf-8", "replace") if raw else None
    root = Path(__file__).resolve().parents[1]
    profile = json.loads((root / "analysis/dump-profile.json").read_text())
    offsets = profile["values"]
    try:
        network = integer(args.engine + offsets["network_client"])
        pawn = integer(args.client + offsets["local_pawn"])
        controller = integer(args.client + offsets["local_controller"])
        result = {"pid": args.pid, "method": "ReadProcessMemory; no writes, target calls or injection",
                  "build": integer(args.engine + offsets["build_number"], "<i"),
                  "network_present": bool(network), "pawn_present": bool(pawn), "controller_present": bool(controller)}
        if network:
            result.update(signon=integer(network + offsets["sign_on"], "<i"),
                          background=integer(network + offsets["background_map"], "<B"),
                          name_528=string(integer(network + 528)), name_536=string(integer(network + 536)))
        if pawn:
            identity = integer(pawn + offsets["identity"])
            result.update(health=integer(pawn + offsets["health"], "<i"),
                          team=integer(pawn + offsets["team"], "<B"),
                          pawn_identity_backlink=integer(identity) == pawn if identity else False)
        reference = pawn or controller
        identity = integer(reference + offsets["identity"]) if reference else None
        system = integer(args.client + offsets["entity_system"])
        handle = integer(identity + 16, "<I") if identity else None
        layouts = []
        if identity and system and handle is not None and handle != 0xffffffff:
            for table in range(16, 65, 8):
                for stride in (0x70, 0x78, 0x80):
                    index = handle & 0x7fff
                    page = integer(system + table + 8 * (index >> 9))
                    entry = page + stride * (index & 511) if page else 0
                    if entry == identity and integer(entry) == reference:
                        layouts.append({"table": hex(table), "stride": hex(stride), "identity_matches": True})
        result["validated_entity_layouts"] = layouts
        if pawn and system and layouts:
            schema = json.loads((Path(profile['source']) / 'client_dll.json').read_text())['client.dll']['classes']
            service = integer(pawn + offsets['weapon_services'])
            weapons = {}
            if service:
                for label, field in [('active', 'm_hActiveWeapon'), ('last', 'm_hLastWeapon')]:
                    candidate = integer(service + schema['CPlayer_WeaponServices']['fields'][field], '<I')
                    if candidate is None or candidate == 0xffffffff: continue
                    index = candidate & 0x7fff
                    page = integer(system + 16 + 8 * (index >> 9))
                    identity = page + (index & 511) * 0x70 if page else 0
                    if not identity or integer(identity + 16, '<I') != candidate: continue
                    instance = integer(identity)
                    if not instance or integer(instance + offsets['identity']) != identity: continue
                    weapons[label] = {'item_id':integer(instance + offsets['attribute_manager'] + offsets['item'] + offsets['item_definition'], '<H'),
                                      'ammo':integer(instance + offsets['clip'], '<i')}
            result['weapon_validation'] = weapons
        (root / "analysis/live-data-validation.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2))
    finally:
        kernel.CloseHandle(process)

if __name__ == "__main__":
    main()
