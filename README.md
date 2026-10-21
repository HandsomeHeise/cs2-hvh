# CS2 HvH

Windows x64 C++20 project with an ImGui/DX11 menu, player ESP, crosshair,
diagnostics, aim settings and cosmetic customization. Menu text is English.
Settings are stored locally in `hvh-menu.ini` beside the DLL.

## Build

On Windows x64 with Python 3.12 or newer and Git:

```powershell
python tools/fetch_deps.py
python tools/build.py --output dist/build
pwsh -File tools/test.ps1 -Dist dist/build -UnitOnly
python -m unittest discover -s tests -p "test_*.py" -v
```

The fetch script downloads pinned, SHA-256-verified ImGui, MinHook and LLVM-MinGW
archives. It installs them inside this checkout; Visual Studio and a CS2
installation are not required to compile or run the isolated test fixtures.
Generated headers needed for building are included under `src/generated`.
Regeneration tools require the corresponding local game files or schema dumps.

For the full DX11 and loader integration suite, run on a Windows desktop:

```powershell
pwsh -File tools/test.ps1 -Dist dist/build
```

The complete local suite currently passes 1,132 C++ checks, ten DX11 cases and
two load/unload cycles. Hosted CI runs the C++ fixtures and Python importer
and release-package tests. Tests requiring local extraction reports are
explicitly skipped when those reports are absent. CI does not verify rendering
or behavior inside a running game.

## Automated builds and releases

GitHub Actions builds and tests pushes to `main`, pull requests, version tags,
and manual workflow runs. Successful builds produce a Windows x64 ZIP artifact.
Pushing a version tag publishes that ZIP and its SHA-256 checksum as a release:

```powershell
git tag v0.1.1
git push origin v0.1.1
```

The repository contains source, tests, build tools and documentation. Compiled
binaries are distributed through Actions artifacts and Releases. Personal
profiles, logs, backups, raw dumps, reverse-engineering reports, downloaded
dependencies and full game assets are excluded from Git and release packages.
Release ZIPs contain only the DLL, loader, usage instructions, build metadata
and required third-party license notices. GitHub also provides source archives.

## Controls and compatibility

`INSERT` opens or closes the menu. The former `END` deactivation shortcut has
been removed. See [TESTEN.txt](TESTEN.txt) for detailed controls and features.

Native interfaces are pinned to the verified game build. A game update can
require regenerating and verifying those interfaces; the build does not update
them automatically. See [THIRD_PARTY.txt](THIRD_PARTY.txt) for dependency notices.
