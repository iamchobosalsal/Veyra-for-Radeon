"""Prepare an optional OpenNR runtime for the existing Veyra application.

Does not modify an installed player, its profile or its active runtime. Proprietary
models are copied only from an explicitly supplied existing private runtime.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[2]
PIN = "191b97b54175d50c7ec5992b54322dd3eb0ef8ac"


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def stage(build, models, app, output):
    if output.exists():
        raise ValueError("Choose a new stage directory; existing files will not be overwritten")
    required = [build / "veyra_open_nr_small.dll", build / "veyra_open_nr_original.dll",
                models / "dlssnr-amd/dlssnr-small.bin", models / "dlssnr-amd/dlssnr.bin",
                models / "dlssnr-amd/shaders/shader-constants.txt",
                app / "shaders/ParityEncode.dxil", app / "shaders/ParityDecode.dxil"]
    for file in required:
        if not file.is_file():
            raise ValueError(f"Missing prerequisite: {file}")
    target = output / "runtime/amd-nr"
    target.mkdir(parents=True)
    shutil.copy2(required[0], target / "LmxxfNrRuntime.dll")
    shutil.copy2(required[1], target / "LmxxfNrRuntime-Original.dll")
    runtime = target / "open-nr"
    (runtime / "dlssnr-amd").mkdir(parents=True)
    for name in ("dlssnr-small.bin", "dlssnr.bin"):
        shutil.copy2(models / "dlssnr-amd" / name, runtime / "dlssnr-amd" / name)
    # Do not reuse a driver pipeline cache across different GPUs/builds.
    shutil.copytree(models / "dlssnr-amd/shaders", runtime / "dlssnr-amd/shaders")
    (runtime / "shaders").mkdir()
    for name in ("ParityEncode.dxil", "ParityDecode.dxil"):
        shutil.copy2(app / "shaders" / name, runtime / "shaders" / name)
    shutil.copytree(models / "licenses", runtime / "licenses")
    (target / "assets").mkdir()  # Existing player supplies this path to the ABI.
    manifest = {
        "provider": "OpenNR Vulkan", "optional": True, "enabledInUserApp": False,
        "source": "https://github.com/Overclockers/DLSSNR-AMD", "commit": PIN,
        "nativeNvidiaPathUnchanged": True, "amdInferenceValidated": False,
        "modelRedistributionPermitted": False,
        "controls": "Veyra intensity; default SF tone/structure/style; custom controls unavailable in released AMD ABI",
        "input": "linear RGBA16F -> Veyra sRGB8 proxy -> native model composition -> Veyra linear resolve",
        "nrExtentLimit": "1920x1080 pixel budget, maximum width 2560, height 1080; Veyra output may remain 4K",
        "files": {str(p.relative_to(output)).replace("\\", "/"): sha(p)
                  for p in sorted(output.rglob("*")) if p.is_file()},
    }
    (output / "open-nr-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    shutil.copy2(ROOT / "scripts/radeon/manage_open_nr_runtime.py", output / "manage_open_nr_runtime.py")
    print(json.dumps({"stage": str(output), "files": len(manifest["files"]), "active": False}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("build", "models", "app", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    stage(*(getattr(args, name).resolve() for name in ("build", "models", "app", "output")))


if __name__ == "__main__":
    main()
