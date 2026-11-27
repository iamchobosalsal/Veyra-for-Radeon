"""Apply/restore the optional OpenNR runtime in an existing, closed Veyra.

Default is a read-only check. No profiles, NVIDIA runtimes, capture settings or
drivers are modified. This candidate still requires Radeon inference validation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import time
import uuid


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def member(root, name):
    rel = PurePosixPath(name)
    if rel.is_absolute() or ".." in rel.parts or "\\" in name or ":" in name:
        raise ValueError("Unsafe runtime path")
    allowed = name in ("runtime/amd-nr/LmxxfNrRuntime.dll", "runtime/amd-nr/LmxxfNrRuntime-Original.dll") or name.startswith("runtime/amd-nr/open-nr/")
    lexical = root.resolve() / rel
    path = lexical.resolve()
    if not allowed or path != lexical or not path.is_relative_to(root.resolve() / "runtime/amd-nr"):
        raise ValueError("Runtime path escapes its scope")
    return path


def atomic_copy(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        shutil.copy2(source, temporary)
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)


def running_paths():
    command = "[Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false); @(Get-Process veyra_qml_ui -ErrorAction SilentlyContinue | Select-Object Path) | ConvertTo-Json -Compress"
    result = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", command],
                            capture_output=True, text=True, encoding="utf-8", timeout=15,
                            creationflags=subprocess.CREATE_NO_WINDOW, check=True)
    rows = json.loads(result.stdout) if result.stdout.strip() else []
    if isinstance(rows, dict):
        rows = [rows]
    if any(not row.get("Path") for row in rows):
        raise RuntimeError("A Veyra process path cannot be checked; close Veyra before applying")
    return [Path(row["Path"]).resolve() for row in rows]


def validate_app(app, paths):
    if not (app / "veyra_qml_ui.exe").is_file():
        raise ValueError("Select the existing versions/2.2.0 application directory")
    if (app / "veyra_qml_ui.exe").resolve() in paths:
        raise RuntimeError("Veyra is running. It has not been closed or modified; exit it before applying")


def package_files(package):
    manifest = json.loads((package / "open-nr-manifest.json").read_text(encoding="utf-8"))
    if manifest.get("provider") != "OpenNR Vulkan" or not manifest.get("files"):
        raise ValueError("Not an OpenNR package")
    for name, expected in manifest["files"].items():
        if sha(member(package, name)) != expected:
            raise ValueError(f"Package checksum mismatch: {name}")
    return manifest["files"]


def apply(package, app, paths):
    validate_app(app, paths)
    files = package_files(package)
    journal = app / "runtime/amd-nr/open-nr-install.json"
    if journal.exists() or (app / "runtime/amd-nr/open-nr").exists():
        raise ValueError("An OpenNR installation or directory already exists; review/restore it first")
    backup = app / "runtime/amd-nr" / ("open-nr-backup-" + time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8])
    originals = {}
    for name in files:
        target = member(app, name)
        originals[name] = sha(target) if target.is_file() else None
        if target.exists() and not target.is_file():
            raise ValueError(f"Target is not a regular file: {target}")
    backup.mkdir(parents=True)
    for name, digest in originals.items():
        if digest:
            atomic_copy(member(app, name), member(backup, name))
    state = {"app": str(app), "backup": str(backup), "installed": files, "originals": originals,
             "amdInferenceValidated": False, "provider": "OpenNR Vulkan"}
    # Persist recovery before the first replacement, including interrupted installs.
    journal.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    written = []
    try:
        for name in files:
            atomic_copy(member(package, name), member(app, name));written.append(name)
        for name, digest in files.items():
            if sha(member(app, name)) != digest:
                raise RuntimeError(f"Installed checksum mismatch: {name}")
    except Exception:
        for name in reversed(written):
            target = member(app, name)
            if originals[name]:
                atomic_copy(member(backup, name), target)
            else:
                target.unlink(missing_ok=True)
        raise
    return {"applied": True, "backup": str(backup), "amdInferenceValidated": False}


def restore(app, paths):
    validate_app(app, paths)
    journal = app / "runtime/amd-nr/open-nr-install.json"
    state = json.loads(journal.read_text(encoding="utf-8"))
    backup = Path(state["backup"]).resolve()
    if not backup.is_relative_to((app / "runtime/amd-nr").resolve()) or Path(state["app"]).resolve() != app:
        raise ValueError("Recovery paths do not belong to this application")
    # Accept untouched/original files after an interrupted or rolled-back apply.
    for name, digest in state["installed"].items():
        target = member(app, name);original = state["originals"][name]
        current = sha(target) if target.is_file() else None
        if current not in (digest, original):
            raise ValueError(f"File changed after installation; preserving it: {name}")
        if original and sha(member(backup, name)) != original:
            raise ValueError(f"Backup checksum mismatch: {name}")
    for name, original in state["originals"].items():
        target = member(app, name)
        if original:
            atomic_copy(member(backup, name), target)
        else:
            target.unlink(missing_ok=True)
    runtime = app / "runtime/amd-nr/open-nr"
    # Only directories created for listed package files, never traverse an
    # arbitrary user directory or junction added after installation.
    directories = set()
    for name in state["installed"]:
        parent = member(app, name).parent
        while parent.is_relative_to(runtime):
            directories.add(parent);parent = parent.parent
    for directory in sorted(directories, key=lambda p: len(p.parts), reverse=True):
        try: directory.rmdir()
        except OSError: pass
    os.replace(journal, backup / "restored-install.json")
    return {"restored": True, "backupKept": str(backup)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--package", type=Path, default=Path(__file__).resolve().parent)
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--apply", action="store_true");action.add_argument("--restore", action="store_true")
    args = parser.parse_args();app = args.app.resolve();package = args.package.resolve()
    paths = running_paths()
    if args.apply: result = apply(package, app, paths)
    elif args.restore: result = restore(app, paths)
    else:
        files = package_files(package)
        result = {"checked": len(files), "appRunning": (app / "veyra_qml_ui.exe").resolve() in paths,
                  "changesMade": False, "amdInferenceValidated": False}
    print(json.dumps(result))


if __name__ == "__main__":
    main()
