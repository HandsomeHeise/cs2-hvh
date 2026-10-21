"""Read PE headers, imports, exports and selected literals without loading the image."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


class PE:
    def __init__(self, path):
        self.path = Path(path).resolve()
        self.data = self.path.read_bytes()
        if self.data[:2] != b"MZ":
            raise ValueError(f"Not an MZ image: {self.path}")
        pe = self.u32(0x3C)
        if self.data[pe:pe + 4] != b"PE\0\0":
            raise ValueError(f"Not a PE image: {self.path}")
        self.machine, count = self.unpack("<HH", pe + 4)
        optional_size = self.u16(pe + 20)
        opt = pe + 24
        magic = self.u16(opt)
        if magic not in (0x10B, 0x20B):
            raise ValueError(f"Unsupported optional header: {magic:#x}")
        self.bits = 64 if magic == 0x20B else 32
        self.base = self.unpack("<Q" if self.bits == 64 else "<I", opt + (24 if self.bits == 64 else 28))[0]
        self.entry = self.u32(opt + 16)
        self.header_size = self.u32(opt + 60)
        ndir = min(self.u32(opt + (108 if self.bits == 64 else 92)), 16)
        directory_offset = opt + (112 if self.bits == 64 else 96)
        self.directories = [self.unpack("<II", directory_offset + i * 8) for i in range(ndir)]
        self.sections = []
        for i in range(count):
            section = opt + optional_size + i * 40
            virtual_size, rva, raw_size, offset = self.unpack("<IIII", section + 8)
            self.sections.append({
                "name": self.data[section:section + 8].split(b"\0")[0].decode("ascii", "replace"),
                "rva": rva, "virtual_size": virtual_size,
                "file_offset": offset, "file_size": raw_size,
            })

    def unpack(self, fmt, offset):
        return struct.unpack_from(fmt, self.data, offset)

    def u16(self, offset):
        return self.unpack("<H", offset)[0]

    def u32(self, offset):
        return self.unpack("<I", offset)[0]

    def offset(self, rva):
        if 0 <= rva < min(self.header_size, len(self.data)):
            return rva
        for s in self.sections:
            delta = rva - s["rva"]
            if 0 <= delta < s["file_size"]:
                result = s["file_offset"] + delta
                if result < len(self.data):
                    return result
        raise ValueError(f"RVA has no file bytes: {rva:#x}")

    def rva(self, offset):
        if offset < self.header_size:
            return offset
        for s in self.sections:
            delta = offset - s["file_offset"]
            if 0 <= delta < s["file_size"]:
                return s["rva"] + delta
        return None

    def cstr(self, offset):
        end = self.data.index(b"\0", offset, min(len(self.data), offset + 32768))
        return self.data[offset:end].decode("ascii", "replace")

    def directory(self, index):
        return self.directories[index] if index < len(self.directories) else (0, 0)

    def imports(self):
        rva, size = self.directory(1)
        if not rva:
            return []
        start = self.offset(rva)
        result = []
        for pos in range(start, start + size, 20):
            original, timestamp, forwarder, name, first = self.unpack("<IIIII", pos)
            if not any((original, timestamp, forwarder, name, first)):
                break
            step = self.bits // 8
            thunk = self.offset(original or first)
            symbols = []
            for i in range(65536):
                value = self.unpack("<Q" if self.bits == 64 else "<I", thunk + step * i)[0]
                if not value:
                    break
                if value & (1 << (self.bits - 1)):
                    symbols.append({"ordinal": value & 0xFFFF})
                else:
                    symbols.append({"name": self.cstr(self.offset(value) + 2)})
            else:
                raise ValueError("Import thunk exceeds inspection limit")
            result.append({"module": self.cstr(self.offset(name)), "symbols": symbols})
        return result

    def exports(self):
        rva, size = self.directory(0)
        if not rva:
            return []
        start = self.offset(rva)
        base, function_count, name_count, functions, names, ordinals = self.unpack("<IIIIII", start + 16)
        labels = {}
        for i in range(name_count):
            index = self.u16(self.offset(ordinals) + i * 2)
            labels.setdefault(index, []).append(self.cstr(self.offset(self.u32(self.offset(names) + i * 4))))
        result = []
        for i in range(function_count):
            address = self.u32(self.offset(functions) + i * 4)
            if address:
                item = {"ordinal": base + i, "names": labels.get(i, []), "rva": hex(address)}
                if rva <= address < rva + size:
                    item["forwarder"] = self.cstr(self.offset(address))
                result.append(item)
        return result

    def literals(self):
        candidate = re.compile(r"(?:Source2Client|Source2EngineToClient|Source2EngineToServer|InputSystemVersion|SchemaSystem_|PanoramaUIEngine|RenderSystem|VEngineCvar|GameResourceServiceClient)[A-Za-z_0-9]*|D3D11CreateDevice(?:AndSwapChain)?|CreateDXGIFactory[0-9]*|Source2Main")
        result = []
        for match in re.finditer(rb"[\x20-\x7e]{4,}", self.data):
            value = match.group().decode("ascii")
            if candidate.fullmatch(value):
                rva = self.rva(match.start())
                result.append({"value": value, "file_offset": hex(match.start()), "rva": hex(rva) if rva is not None else None})
        return result

    def inspect(self):
        return {
            "path": str(self.path), "bytes": len(self.data),
            "sha256": hashlib.sha256(self.data).hexdigest(),
            "machine": hex(self.machine), "bits": self.bits,
            "image_base": hex(self.base), "entry_rva": hex(self.entry),
            "sections": self.sections, "imports": self.imports(),
            "exports": self.exports(), "candidate_literals": self.literals(),
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    images = [PE(path).inspect() for path in args.files]
    result = {
        "method": "Static PE parsing using Python standard library; no target execution",
        "limitations": [
            "Imports cover the normal import directory; delay-loaded and dynamically resolved symbols are not enumerated.",
            "Literal matches are candidates, not verified registered interfaces or callable contracts.",
            "RVA and preferred image base are file identities, not live process addresses.",
        ],
        "images": images,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    for image in images:
        print(f"{Path(image['path']).name}: {image['bytes']} bytes, {image['bits']} bit, {len(image['exports'])} exports")
        print("  Imports: " + ", ".join(item["module"] for item in image["imports"]))
        print("  Candidates: " + ", ".join(sorted({item["value"] for item in image["candidate_literals"]})))


if __name__ == "__main__":
    main()
