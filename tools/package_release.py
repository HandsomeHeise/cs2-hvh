"""Create a minimal release ZIP from explicitly allowed build outputs."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def package(distribution, output, version, commit):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,79}", version):
        raise ValueError("Invalid release version")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Expected a full Git commit SHA")
    distribution = Path(distribution).resolve()
    output = Path(output).resolve()
    files = {
        "hvh_menu.dll": distribution / "hvh_menu.dll",
        "hvh_loader.exe": distribution / "hvh_loader.exe",
        "IMGUI-LICENSE.txt": distribution / "IMGUI-LICENSE.txt",
        "MINHOOK-LICENSE.txt": distribution / "MINHOOK-LICENSE.txt",
        "README.md": ROOT / "README.md",
        "TESTEN.txt": ROOT / "TESTEN.txt",
        "THIRD_PARTY.txt": ROOT / "THIRD_PARTY.txt",
    }
    notices = distribution / "licenses"
    files["licenses/LLVM-MinGW-LICENSE.txt"] = notices / "LLVM-MinGW-LICENSE.txt"
    for path in sorted(notices.glob("COPYING*")):
        if path.is_file():
            files["licenses/" + path.name] = path
    if not any(name.startswith("licenses/COPYING") for name in files):
        raise FileNotFoundError("Missing MinGW runtime license notices")
    contents = {name: path.read_bytes() for name, path in files.items()}
    contents["BUILD-INFO.json"] = (json.dumps({
        "version": version, "commit": commit, "platform": "windows-x64",
        "toolchain": "llvm-mingw-20261006",
    }, indent=2) + "\n").encode()
    contents["SHA256SUMS.txt"] = ("".join(
        hashlib.sha256(data).hexdigest() + "  " + name + "\n"
        for name, data in sorted(contents.items())
    )).encode()
    output.mkdir(parents=True, exist_ok=True)
    archive = output / ("cs2-hvh-" + version + "-windows-x64.zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as dest:
        for name, data in sorted(contents.items()):
            dest.writestr(name, data)
    with zipfile.ZipFile(archive) as check:
        if check.testzip() is not None or set(check.namelist()) != set(contents):
            raise ValueError("Release ZIP verification failed")
    checksum = archive.with_suffix(archive.suffix + ".sha256")
    checksum.write_text(hashlib.sha256(archive.read_bytes()).hexdigest() + "  " + archive.name + "\n", encoding="utf-8")
    print("Release package:", archive)
    return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--commit", required=True)
    args = parser.parse_args()
    package(args.dist, args.output, args.version, args.commit)


if __name__ == "__main__":
    main()
