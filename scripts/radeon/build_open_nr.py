"""Build the optional Veyra OpenNR runtime and isolated diagnostic tools.

All dependencies and products are external to Git. Models are user supplied.
"""
import argparse
import json
import re
import shutil
import subprocess
from pathlib import Path

PIN = "191b97b54175d50c7ec5992b54322dd3eb0ef8ac"
ROOT = Path(__file__).resolve().parents[2]


def run(args, timeout=900):
    subprocess.run([str(x) for x in args], check=True, timeout=timeout)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--deps", type=Path, default=Path("C:/capture/deps"))
    ap.add_argument("--output", type=Path, default=Path("C:/capture/evidence/veyra-open-nr"))
    ap.add_argument("--prepare-shaders", action="store_true", help="Build a fresh private shader directory")
    ap.add_argument("--model", type=Path, help="Import the user's own nvngx_dlssnr.dll into the private runtime")
    a = ap.parse_args()
    a.deps = a.deps.resolve()
    a.output = a.output.resolve()
    source = a.deps / "dlssnr-amd-veyra-source"
    pin = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if pin != PIN:
        raise SystemExit("Upstream revision differs; review before updating the adapter")
    run(["git", "-C", source, "diff", "--exit-code", "--quiet"])
    tool = a.deps / "llvm-mingw-20261117-ucrt-x86_64/bin"
    cmake = a.deps / "cmake-4.5.1-windows-x86_64/bin/cmake.exe"
    generated = a.output / "generated"
    generated.mkdir(parents=True, exist_ok=True)
    # Resolve the system loader by an absolute system path. This avoids loading
    # a game hook or app-local vulkan-1.dll. No machine PATH/registry changes.
    exports = subprocess.check_output([str(tool / "llvm-readobj.exe"), "--coff-exports",
                                       "C:/Windows/System32/vulkan-1.dll"], text=True)
    names = sorted(set(re.findall(r"Name: (vk\w+)", exports)))
    if "vkCreateInstance" not in names:
        raise SystemExit("System Vulkan loader exports unavailable")
    cpp = ['#include <windows.h>', '#include <stdexcept>', '#include <mutex>', 'extern "C" {']
    cpp += [f'FARPROC veyra_p_{n} = nullptr;' for n in names]
    cpp += ['}', 'extern "C" void nr_vk_load() {', 'static std::once_flag once;', 'std::call_once(once, [] {',
            'wchar_t path[32768]{};', 'UINT n=GetSystemDirectoryW(path, 32768);',
            'if(!n || n>32730) throw std::runtime_error("System directory unavailable");',
            'wcscat_s(path,L"\\\\vulkan-1.dll");',
            'HMODULE loader=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);',
            'if(!loader) throw std::runtime_error("System Vulkan loader unavailable");']
    cpp += [f'veyra_p_{n}=GetProcAddress(loader,"{n}"); if(!veyra_p_{n}) throw std::runtime_error("Missing {n}");' for n in names]
    cpp += ['});', '}']
    (generated / "vulkan_dispatch.cpp").write_text("\n".join(cpp)+"\n")
    asm = ['.text']
    for n in names:
        asm += [f'.globl {n}', f'{n}:', f'jmp *veyra_p_{n}(%rip)']
    (generated / "vulkan_dispatch.s").write_text("\n".join(asm)+"\n")
    run([cmake, "-S", ROOT / "tests/radeon/open_nr", "-B", a.output / "build", "-G", "MinGW Makefiles",
         "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_CXX_COMPILER={tool / 'clang++.exe'}",
         f"-DCMAKE_ASM_COMPILER={tool / 'clang.exe'}", f"-DCMAKE_MAKE_PROGRAM={tool / 'mingw32-make.exe'}",
         f"-DOPEN_NR_SOURCE={source}", f"-DVULKAN_HEADERS={a.deps / 'vulkan-headers-1.4.357'}",
         f"-DOPEN_NR_GENERATED={generated}"])
    run([cmake, "--build", a.output / "build", "--parallel", "2"])
    runtime = a.output / "runtime/dlssnr-amd"
    if a.prepare_shaders:
        shaders = runtime / "shaders"
        if shaders.exists():
            raise SystemExit("Shaders already exist; choose a new private output directory")
        compiler = a.deps / "glslang-16.5.0/bin"
        upstream_bin = source / "toolchain/glslang/bin"
        upstream_bin.mkdir(parents=True, exist_ok=True)
        for file in compiler.iterdir():
            if file.is_file():
                shutil.copy2(file, upstream_bin / file.name)
        run([os_python(), source / "windows/build/build_network.py", "rdna4", "--out", shaders])
    if a.model:
        runtime.mkdir(parents=True, exist_ok=True)
        run([a.output / "build/open_nr_model_import.exe", a.model.resolve(strict=True), runtime], timeout=120)
    licenses = a.output / "runtime/licenses"
    licenses.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / "LICENSE", licenses / "DLSSNR-AMD-MIT.txt")
    shutil.copy2(source / "windows/shaders/passes/include/fsr1/LICENSE.txt", licenses / "AMD-FSR1-MIT.txt")
    (a.output / "source.json").write_text(json.dumps({"repository": "https://github.com/Overclockers/DLSSNR-AMD",
        "commit": pin, "license": "MIT", "vulkanHeaders": "e3b1eec08173d6b825cd3ac88c885a63b621504a",
        "inference": "source-built", "redistributeModel": False,
        "privateModelPresent": (runtime / "dlssnr-small.bin").is_file(), "gameHook": False}, indent=2)+"\n")


def os_python():
    import sys
    return sys.executable


if __name__ == "__main__":
    main()
