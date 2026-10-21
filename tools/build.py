"""Build the x64 DLL, loader and DX11 smoke-test host with the local toolchain."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import os
import argparse
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
DIST = ROOT / "dist"
BIN = ROOT / ".toolchain" / "llvm-mingw" / "bin"
INCLUDES = [ROOT / p for p in ("include", "src", "external/imgui", "external/imgui/backends", "external/minhook/include")]
FLAGS = ["-O2", "-g", "-fms-extensions", "-DUNICODE", "-D_UNICODE", "-DNOMINMAX", "-D_WIN32_WINNT=0x0A00"]
LIBS = ["-ld3d11", "-ldxgi", "-ld3dcompiler", "-ldwmapi", "-limm32", "-lgdi32", "-luser32"]


def run(args):
    subprocess.run([str(x) for x in args], cwd=ROOT, check=True)


def compile_one(relative):
    source = ROOT / relative
    obj = BUILD / (relative.replace("/", "_") + ".o")
    headers = [p for folder in INCLUDES for p in folder.rglob("*.h*")]
    newest = max([source.stat().st_mtime] + [p.stat().st_mtime for p in headers])
    if relative in ("tests/server_rules.cpp","tests/server_rule_filters.cpp"):
        newest = max(newest,(ROOT/"src/server_rules.cpp").stat().st_mtime,(ROOT/"tests/server_rules.cpp").stat().st_mtime)
    if relative=="tests/esp.cpp":
        newest=max(newest,(ROOT/"src/aim.cpp").stat().st_mtime,(ROOT/"tests/aim_runtime.cpp").stat().st_mtime)
    if relative == "tests/skin_transactions.cpp" or (relative.startswith("tests/aim_") and relative not in ("tests/aim_core.cpp","tests/aim_spread.cpp")):
        implementation = "skins.cpp" if relative.endswith("skin_transactions.cpp") else "aim.cpp"
        newest = max(newest, (ROOT / "src" / implementation).stat().st_mtime)
        if relative != "tests/skin_transactions.cpp":
            newest = max(newest,(ROOT / "tests/aim_runtime.cpp").stat().st_mtime,(ROOT / "tests/aim_ballistics.cpp").stat().st_mtime)
    if obj.exists() and obj.stat().st_mtime >= newest:
        return obj
    cpp = source.suffix == ".cpp"
    compiler = BIN / ("x86_64-w64-mingw32-clang++.exe" if cpp else "x86_64-w64-mingw32-clang.exe")
    print("Compile", relative, flush=True)
    run([compiler, "-std=c++20" if cpp else "-std=c11", *FLAGS, *["-I" + str(p) for p in INCLUDES], "-c", source, "-o", obj])
    return obj


def main():
    global DIST
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=DIST)
    args = parser.parse_args()
    DIST = args.output.resolve()
    if not BIN.exists() or not (ROOT / "external/imgui/imgui.h").exists():
        raise SystemExit("Run python tools/fetch_deps.py first.")
    BUILD.mkdir(exist_ok=True); DIST.mkdir(parents=True, exist_ok=True)
    overlay = ["src/overlay.cpp", "src/menu.cpp", "src/config.cpp", "src/game_data.cpp", "src/skins.cpp", "src/aim.cpp", "src/server_rules.cpp", "src/esp.cpp"]
    overlay += ["external/imgui/" + name + ".cpp" for name in ("imgui", "imgui_draw", "imgui_tables", "imgui_widgets")]
    overlay += ["external/imgui/backends/imgui_impl_win32.cpp", "external/imgui/backends/imgui_impl_dx11.cpp"]
    overlay += ["external/minhook/src/" + name + ".c" for name in ("buffer", "hook", "trampoline", "hde/hde64")]
    aim_fixtures = ["core","spread","runtime","ballistics","actions","smoke","chance","hitboxes","performance"]
    sources = overlay + ["src/loader.cpp", "tests/dx11_host.cpp", "tests/features.cpp", "tests/skin_transactions.cpp", "tools/diagnostic_probe.cpp", "tests/server_rules.cpp", "tests/server_rule_filters.cpp", "tests/esp.cpp"] + [f"tests/aim_{fixture}.cpp" for fixture in aim_fixtures]
    with ThreadPoolExecutor(max_workers=min(os.cpu_count() or 4, 6)) as pool:
        objects = dict(zip(sources, pool.map(compile_one, sources)))
    compiler = BIN / "x86_64-w64-mingw32-clang++.exe"
    run([compiler, "-shared", "-static", *[objects[s] for s in overlay], *LIBS,
         "-Wl,--exclude-all-symbols", "-Wl,--out-implib," + str(BUILD / "hvh_menu.a"), "-o", DIST / "hvh_menu.dll"])
    run([compiler, "-static", "-municode", objects["src/loader.cpp"], "-lpsapi", "-o", DIST / "hvh_loader.exe"])
    run([compiler, "-static", "-municode", "-mwindows", objects["tests/dx11_host.cpp"], *LIBS, "-o", DIST / "dx11_test_host.exe"])
    run([compiler, "-static", objects["tests/features.cpp"], objects["src/config.cpp"], objects["src/game_data.cpp"], "-o", DIST / "feature_tests.exe"])
    run([compiler, "-static", objects["tools/diagnostic_probe.cpp"], objects["src/game_data.cpp"], "-o", DIST / "diagnostic_probe.exe"])
    minhook = [objects[s] for s in overlay if s.startswith("external/minhook/")]
    for fixture in ("aim_core", "aim_spread"):
        run([compiler, "-static", objects[f"tests/{fixture}.cpp"], "-o", DIST / f"{fixture}_tests.exe"])
    for fixture in ("aim_runtime", "aim_ballistics", "aim_actions", "aim_smoke", "aim_chance", "aim_hitboxes", "aim_performance"):
        run([compiler,"-static",objects[f"tests/{fixture}.cpp"],objects["src/config.cpp"],objects["src/game_data.cpp"],objects["src/server_rules.cpp"],objects["src/esp.cpp"],*minhook,"-o",DIST/f"{fixture}_tests.exe"])
    run([compiler,"-static",objects["tests/esp.cpp"],objects["src/config.cpp"],objects["src/game_data.cpp"],objects["src/server_rules.cpp"],objects["src/esp.cpp"],*minhook,"-o",DIST/"esp_tests.exe"])
    for fixture in ("server_rules", "server_rule_filters"):
        run([compiler,"-static",objects[f"tests/{fixture}.cpp"],objects["src/config.cpp"],objects["src/game_data.cpp"],*minhook,"-o",DIST/f"{fixture}_tests.exe"])
    run([compiler, "-static", objects["tests/skin_transactions.cpp"], objects["src/config.cpp"], objects["src/game_data.cpp"], *minhook,
         "-o", DIST / "skin_transaction_tests.exe"])
    for name, source in [("IMGUI-LICENSE.txt", "external/imgui/LICENSE.txt"), ("MINHOOK-LICENSE.txt", "external/minhook/LICENSE.txt")]:
        shutil.copyfile(ROOT / source, DIST / name)
    shutil.copyfile(ROOT / "TESTEN.txt", DIST / "TESTEN.txt")
    shutil.copyfile(ROOT / "THIRD_PARTY.txt", DIST / "THIRD_PARTY.txt")
    notices = DIST / "licenses"
    notices.mkdir(exist_ok=True)
    toolchain = ROOT / ".toolchain/llvm-mingw"
    shutil.copyfile(toolchain / "LICENSE.TXT", notices / "LLVM-MinGW-LICENSE.txt")
    for notice in (toolchain / "x86_64-w64-mingw32/share/mingw32").glob("COPYING*"):
        shutil.copyfile(notice, notices / notice.name)
    print("Built DLL, loader, DX11 host, feature tests and read-only probe in", DIST, flush=True)


if __name__ == "__main__":
    main()
