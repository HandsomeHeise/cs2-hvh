"""Fetch pinned upstream sources and a project-local Windows x64 toolchain."""
from pathlib import Path
import hashlib
import shutil
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = {
    "imgui": ("ocornut/imgui", "f1cc2ae15e53a861a874c3034aae6798fde194ab"),
    "minhook": ("TsudaKageyu/minhook", "c3fcafdc10146beb5919319d0683e44e3c30d537"),
}
DIGESTS = {
    "imgui-f1cc2ae15e53a861a874c3034aae6798fde194ab.zip": "008e1f3398e1c6e4d317ae4a05d2dec597bd360f4f3717b1d96e9edb66dba34f",
    "minhook-c3fcafdc10146beb5919319d0683e44e3c30d537.zip": "cdcb160f734d81bd4d235dfea79e3f5a661c8ef0ab74fa814272aa5449069034",
    "llvm-mingw-20261006-ucrt-x86_64.zip": "317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a",
}


def fetch(url, archive):
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        partial = archive.with_suffix(".partial")
        print("Downloading", url, flush=True)
        request = urllib.request.Request(url, headers={"User-Agent": "hvh-menu-build/1.0"})
        with urllib.request.urlopen(request, timeout=120) as response, partial.open("wb") as out:
            shutil.copyfileobj(response, out)
        partial.replace(archive)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != DIGESTS[archive.name]:
        raise ValueError("Archive SHA-256 mismatch: " + archive.name)
    print(archive.name, "sha256=" + digest, flush=True)


def extract(archive, destination):
    destination.mkdir(parents=True, exist_ok=True)
    root = destination.resolve()
    with zipfile.ZipFile(archive) as source:
        # Strip the single archive root. Reject traversal before writing any file.
        members = [(item, Path(*Path(item.filename).parts[1:])) for item in source.infolist()]
        for item, relative in members:
            target = (root / relative).resolve()
            if not target.is_relative_to(root):
                raise ValueError("Unsafe archive path: " + item.filename)
        for item, relative in members:
            if not relative.parts:
                continue
            target = root / relative
            if item.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.open(item) as inp, target.open("wb") as out:
                    shutil.copyfileobj(inp, out)


def main():
    for name, (repo, commit) in SOURCES.items():
        destination = ROOT / "external" / name
        marker = destination / ".source-commit"
        if marker.exists() and marker.read_text().strip() == commit:
            continue
        archive = ROOT / ".downloads" / (name + "-" + commit + ".zip")
        fetch("https://codeload.github.com/" + repo + "/zip/" + commit, archive)
        extract(archive, destination)
        marker.write_text(commit + "\n")
    destination = ROOT / ".toolchain" / "llvm-mingw"
    if not (destination / "bin" / "x86_64-w64-mingw32-clang++.exe").exists():
        archive = ROOT / ".downloads" / "llvm-mingw-20261006-ucrt-x86_64.zip"
        fetch("https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/" + archive.name, archive)
        extract(archive, destination)


if __name__ == "__main__":
    main()
