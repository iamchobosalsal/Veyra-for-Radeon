"""Prepare a private Radeon preview without touching the donor or launching it.

The default command only prints its plan. --prepare explicitly copies files and
creates an unsigned review ZIP. --with-local-nr-assets separately copies the
user's existing assets to the installed preview, never into that ZIP.
The three required FidelityFX SDK DLLs are immutable versioned dependencies;
their exact runtime_local/amd/fidelityfx paths are included in the ZIP.
"""
import argparse
import json
from pathlib import Path
import shutil
import tempfile
import zipfile

import package_release as release

PROVIDER_SHA256 = "94907a7c34928970608dbf4f9b7b38b13c474864a466ac2d1ffbaac6adccf6c4"
ROOT_DLLS = {
    "avcodec-63.dll", "avfilter-12.dll", "avformat-63.dll", "avutil-61.dll",
    "D3Dcompiler_47.dll", "dav1d.dll", "dolby_vision.dll", "libcrypto-3-x64__.dll",
    "msvcp140.dll", "swresample-7.dll", "swscale-10.dll", "vcruntime140_1.dll",
    "vcruntime140.dll", "WinPixEventRuntime.dll", "zlib1__.dll",
}
PLUGIN_DIRS = ("iconengines", "imageformats", "networkinformation", "platforms", "styles", "tls")
NR_FILES = ("LmxxfNrRuntime.dll", "LmxxfNrRuntime-Original.dll", "LICENSE-lmxxf-MIT.txt", "HIP-API-LICENSE.txt")
RIFE_FILES = ("nvngx_dlssg.dll", "DirectML.dll", "flownet.bin", "flownet.param")
PRIVATE_PARTS = {"logs", "shader-cache", "runtime_local", "media", ".git", ".cache", "capture-profile"}


def tree_files(folder):
    if not folder.is_dir():
        raise ValueError(f"required folder missing: {folder}")
    for path in sorted(folder.rglob("*")):
        if path.is_file():
            yield path


def build_plan(donor, source, build):
    """Explicit categories only: never recursively copy the donor root/runtime."""
    plan = {}

    def add(path, target):
        release.check_path(target)
        if not path.is_file():
            raise ValueError(f"required input missing: {path}")
        if path.suffix.lower() in {".log", ".bak", ".tmp", ".ini", ".pem", ".key", ".pfx"}:
            raise ValueError(f"unexpected private/config file: {path}")
        plan[target] = path

    def tree(folder, target, extensions=None):
        for path in tree_files(folder):
            relative = path.relative_to(folder)
            if set(p.lower() for p in relative.parts) & PRIVATE_PARTS:
                continue
            if extensions is None or path.suffix.lower() in extensions:
                add(path, (Path(target) / relative).as_posix())

    for name in sorted(ROOT_DLLS):
        add(donor / name, name)
    qt_dlls = sorted(donor.glob("Qt6*.dll"))
    if not qt_dlls:
        raise ValueError("donor Qt runtime is missing")
    for path in qt_dlls:
        add(path, path.name)
    add(donor / "qt.conf", "qt.conf")
    for directory in PLUGIN_DIRS:
        tree(donor / directory, directory, {".dll"})
    tree(donor / "qml", "qml")
    # Keep Qt imports and any donor-only UI assets; the accepted source tree wins.
    tree(source / "qml" / "Veyra", "qml/Veyra")
    tree(donor / "assets", "assets", {".png", ".svg", ".webp", ".jpg", ".jpeg"})
    tree(donor / "licenses", "licenses")
    for name in ("LICENSE", "THIRD_PARTY_NOTICES.md", "README-RADEON.ko.md", "RELEASE-RADEON.ko.md"):
        add(source / name, name)
    add(source / "docs" / "radeon" / "PACKAGING.md", "docs/radeon/PACKAGING.md")
    # Translation code/catalog are embedded by the new build. Copy its source
    # catalog as the external companion too; never retain the donor catalog.
    add(source / "i18n" / "catalog.json", "i18n/catalog.json")
    add(build / release.PLAYER, release.PLAYER)
    tree(build / "shaders", "shaders", {".dxil", ".cso", ".qsb"})
    if not any(name.startswith("shaders/") for name in plan):
        raise ValueError("new build shader set is missing")
    for relative in release.FSR_RUNTIME_FILES:
        add(donor / relative, relative)
    for name in NR_FILES:
        add(donor / "runtime" / "amd-nr" / name, "runtime/amd-nr/" + name)
    for name in RIFE_FILES:
        add(donor / "runtime" / "experimental" / "anr" / name, "runtime/experimental/anr/" + name)
    if release.digest(plan[release.PROVIDER]) != PROVIDER_SHA256:
        raise ValueError("RIFE provider differs from the explicitly accepted donor build")
    if len({name.lower() for name in plan}) != len(plan):
        raise ValueError("case-colliding stage paths")
    return plan


def copy_verified(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    # New destination files, no hardlinks or in-place edits to donor/source.
    shutil.copyfile(source, target)
    if target.stat().st_size != source.stat().st_size or release.digest(target) != release.digest(source):
        raise ValueError(f"copy verification failed: {target}")


def require_new_disjoint(paths, inputs):
    resolved = [p.resolve() for p in paths]
    if any(p.exists() for p in resolved):
        raise FileExistsError("stage/output/preview must all be new paths")
    for i, path in enumerate(resolved):
        for other in resolved[i + 1:] + [p.resolve(strict=True) for p in inputs]:
            if path == other or path in other.parents or other in path.parents:
                raise ValueError("outputs and source inputs must be separate, non-nested directories")


def prepare(donor, source, build, stage, output, preview, version, local_assets=False):
    version = release.canonical_version(version)
    require_new_disjoint([stage, output, preview], [donor, source, build])
    trust = release.trust_policy(json.loads((build / "portable-update-trust.json").read_bytes()), True)
    if trust.get("installEnabled") is not False or trust.get("repository") or trust.get("publicKey"):
        raise ValueError("this private preview must have an unconfigured, disabled updater")
    plan = build_plan(donor, source, build)
    built = {"Veyra.exe": build / "veyra_portable_launcher.exe", release.WORKER: build / release.WORKER}
    build_identities = {name: {"sha256": release.digest(path), "size": path.stat().st_size}
                        for name, path in built.items()}
    stage.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="radeon-preview-stage-", dir=stage.parent) as temporary:
        scratch = Path(temporary) / "stage"
        scratch.mkdir()
        for name, path in plan.items():
            copy_verified(path, scratch / name)
        audit = {
            "schema": 1, "product": release.PRODUCT, "version": version,
            "gpuPackage": "AMD", "releaseReady": False, "localOnly": True,
            "radeonRedistributionReviewed": False,
            "purpose": "Private local review only; public redistribution has not been reviewed.",
            "buildArtifacts": build_identities,
            "radeonSourceBuiltProviderSha256": PROVIDER_SHA256,
            "files": [{"path": name, "size": (scratch / name).stat().st_size,
                       "sha256": release.digest(scratch / name),
                       "origin": "local source/build" if path.is_relative_to(source) or path.is_relative_to(build)
                                 else "existing local Radeon runtime donor",
                       "redistribution": {"reviewed": False}}
                      for name, path in sorted(plan.items())],
        }
        release.validate_audit(audit, version, None, review=True)
        (scratch / "package-manifest.json").write_bytes(release.encode(audit))
        scratch.rename(stage)
    bundle = release.wrap(stage, build, output, version, review=True)
    inspection = release.inspect_bundle(bundle, version, review=True)
    preview.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="radeon-preview-install-", dir=preview.parent) as temporary:
        install = Path(temporary) / "preview"
        install.mkdir()
        prefix = release.stem(version) + "/"
        # Only this freshly produced and inspected ZIP is unpacked here.
        with zipfile.ZipFile(bundle) as archive:
            for info in archive.infolist():
                relative = info.filename[len(prefix):]
                if not release.safe_path(relative):
                    raise ValueError("unsafe inspected ZIP entry")
                target = install / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(info) as incoming, target.open("xb") as outgoing:
                    shutil.copyfileobj(incoming, outgoing)
        local_inventory = []
        if local_assets:
            assets = donor / "runtime" / "amd-nr" / "assets"
            for path in tree_files(assets):
                relative = path.relative_to(assets)
                if set(p.lower() for p in relative.parts) & PRIVATE_PARTS or path.suffix.lower() in {".log", ".bak", ".tmp"}:
                    continue
                if not release.safe_path(relative.as_posix()):
                    raise ValueError("unsafe local asset path")
                target = install / "runtime_local" / "radeon" / "nr-assets" / relative
                copy_verified(path, target)
                local_inventory.append({"path": relative.as_posix(), "size": target.stat().st_size,
                                        "sha256": release.digest(target)})
            metadata = install / "runtime_local" / "radeon" / "local-assets-manifest.json"
            metadata.parent.mkdir(parents=True, exist_ok=True)
            metadata.write_bytes(release.encode({"localOnly": True, "notIncludedInZip": True, "files": local_inventory}))
        for name in ("README-RADEON.ko.md", "RELEASE-RADEON.ko.md"):
            copy_verified(install / "versions" / version / name, install / name)
        install.rename(preview)
    return {"launcher": str(preview / "Veyra.exe"), "reviewZip": str(bundle),
            "stage": str(stage), "localAssetFiles": len(local_inventory), "inspection": inspection}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("donor", "source", "build", "stage", "output", "preview"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--version", default="1.0.0")
    parser.add_argument("--prepare", action="store_true")
    parser.add_argument("--with-local-nr-assets", action="store_true")
    args = parser.parse_args()
    if not args.prepare:
        require_new_disjoint([args.stage, args.output, args.preview], [args.donor, args.source, args.build])
        plan = build_plan(args.donor, args.source, args.build)
        print(json.dumps({"plannedStageFiles": len(plan), "plannedBytes": sum(p.stat().st_size for p in plan.values()),
                          "preview": str(args.preview), "writesPerformed": False,
                          "localAssetsSeparateFromZip": args.with_local_nr_assets}, indent=2))
        return
    print(json.dumps(prepare(args.donor, args.source, args.build, args.stage, args.output, args.preview,
                             args.version, args.with_local_nr_assets), indent=2))


if __name__ == "__main__":
    main()
