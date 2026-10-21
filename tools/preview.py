"""Convert the test host's RGB framebuffer readback to a PNG (no extra packages)."""
from pathlib import Path
import struct
import sys
import zlib

def convert(source, destination):
    header, pixels = Path(source).read_bytes().split(b"\n255\n", 1)
    if not header.startswith(b"P6\n"):
        raise ValueError("Expected a P6 RGB framebuffer")
    width, height = map(int, header.splitlines()[1].split())
    if len(pixels) != width * height * 3:
        raise ValueError("Unexpected framebuffer byte count")
    def chunk(name, data):
        return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data))
    rows = b"".join(b"\0" + pixels[y * width * 3:(y + 1) * width * 3] for y in range(height))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    Path(destination).write_bytes(png + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))

if __name__ == "__main__":
    convert(*sys.argv[1:])
