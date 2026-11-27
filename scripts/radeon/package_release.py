"""Prepare a reviewed Radeon fork ZIP; sign an update index only on explicit request.

Public manual prereleases keep the updater disabled and still require the complete
public redistribution and corresponding-source audit. They cannot be signed as updates.
No downloads, key generation, GitHub upload, or changes to installed applications.
Publisher review is an input, not a legal conclusion made by this script.
An explicitly declared upstream NR model bundle records byte provenance separately;
that provenance is not a model license or a redistribution-rights conclusion.
"""
import argparse
import base64
from contextlib import contextmanager
import hashlib
import json
from pathlib import Path
import re
import stat
import subprocess
import tempfile
import zipfile

PRODUCT = "Veyra-Radeon-portable"
CHANNEL = "radeon-stable"
INDEX = "Veyra-Radeon-portable-update-v1.json"
MARKER = b"VEYRA_RADEON_PORTABLE_V1\nAMD\n"
MANUAL_NOTICE = (b"Public manual prerelease - experimental Radeon community fork.\n"
                 b"Automatic update checks, downloads and installation are disabled.\n"
                 b"Install reviewed Radeon releases manually in separate folders.\n"
                 b"Never overwrite this folder with an upstream Veyra release.\n")
CORE_NOTICE = (b"Veyra for Radeon Core package.\n"
               b"FidelityFX SDK runtime DLLs are not included.\n"
               b"FSR super resolution and FSR frame generation are unavailable in this package.\n"
               b"Native 4K capture/output is separate from FSR super resolution.\n")
PLAYER = "veyra_qml_ui.exe"
WORKER = "veyra_update_worker.exe"
PROVIDER = "runtime/experimental/anr/nvngx_dlssg.dll"
# Immutable SDK dependencies live beside the versioned player. These exact
# runtime files are not user settings, despite the historical directory name.
FSR_RUNTIME_FILES = (
    "runtime_local/amd/fidelityfx/amd_fidelityfx_loader_dx12.dll",
    "runtime_local/amd/fidelityfx/amd_fidelityfx_upscaler_dx12.dll",
    "runtime_local/amd/fidelityfx/amd_fidelityfx_framegeneration_dx12.dll",
)
OPEN_FSR_RUNTIME_FILE = "runtime_local/amd/fidelityfx/amd_fidelityfx_dx12.dll"
ALL_FSR_RUNTIME_FILES = (*FSR_RUNTIME_FILES, OPEN_FSR_RUNTIME_FILE)
NR_MODEL_PREFIX = "runtime/amd-nr/assets/"
NR_MODEL_ARCHIVE_URL = "https://github.com/Likely7/Veyra-NRVideo/releases/download/v2.2.0/Veyra-2.2.0-AMD-win64-portable.zip"
NR_MODEL_ARCHIVE_SHA256 = "b383cd4fbc94a6b84e32f36bc8c2758b4c03216c2bd0a2560e3112ba520d6905"
NR_STYLE_CONFIG_PATH = NR_MODEL_PREFIX + "native-game-flags.txt"
NR_STYLE_ORIGINAL_SHA256 = "830255fba2ead4d86351c669c0a2cf92e6c99b325bc8296d5946f42dc9a6a97b"
NR_STYLE_ZERO_SHA256 = "33f7647696e0d2b63827e1d1667b4be10be822a2110a55d044963c26cc6a2418"
NR_MODEL_SUFFIXES = {".f16", ".f32", ".i32", ".hsaco", ".cso", ".hlsl", ".hlsli", ".txt", ".json"}
SHA = re.compile(r"[0-9a-f]{64}\Z")
RESERVED = re.compile(r"(?:CON|PRN|AUX|NUL|CONIN\$|CONOUT\$|COM[1-9¹²³]|LPT[1-9¹²³])(?:\.|$)", re.IGNORECASE)


def canonical_version(value):
    parts = value.split(".")
    if len(parts) != 3 or any(not re.fullmatch(r"0|[1-9][0-9]{0,4}", p) or int(p) > 65535 for p in parts):
        raise ValueError("expected a three-part numeric Radeon version")
    return value


def repository(value):
    if (not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9-]{0,38}/[A-Za-z0-9_.-]+", value)
            or value.split("/")[1] in {".", ".."}
            or value.lower() == "likely7/veyra-nrvideo"):
        raise ValueError("a separate Radeon owner/repository is required")
    return value


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def encode(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def safe_path(name):
    return (isinstance(name, str) and 0 < len(name) < 500
            and not re.search(r'[\x00-\x1f\\:"*?<>|]', name)
            and all(p not in {"", ".", ".."} and not p.endswith((" ", "."))
                    and not RESERVED.match(p) for p in name.split("/")))


def check_path(name, nr_models=()):
    if not safe_path(name):
        raise ValueError(f"unsafe package path: {name}")
    lower = name.lower()
    if lower in nr_models:
        check_nr_model_path(name)
        return
    if lower in ALL_FSR_RUNTIME_FILES:
        return
    parts = lower.split("/")
    if (set(parts) & {"runtime_local", "logs", ".git", ".cache", "user-data-2.0.0", "capture-profile", "media"}
            or lower.startswith(("runtime/amd-nr/assets/", "runtime/amd-nr-rdna3/", "runtime/nvidia-vfg/"))
            or "/bootstrap/" in lower or lower.endswith((".f16", ".f32", ".pem", ".key", ".pfx"))
            or parts[-1] in {"neuralhub.ini", "nvcuda.dll", "nvapi64.dll", "amdhip64_7.dll", "winmm.dll"}
            or (parts[-1].startswith("nvngx") and lower != PROVIDER)):
        raise ValueError(f"private or unapproved runtime path: {name}")


def check_nr_model_path(name):
    # This exception is reachable only through the exact model manifest set.
    lower = name.lower() if isinstance(name, str) else ""
    parts = lower.split("/")
    if (not safe_path(name) or not name.startswith(NR_MODEL_PREFIX)
            or set(parts) & {"runtime_local", "logs", "cache", ".cache", "shader-cache", ".git",
                              "profile", "profiles", "userdata", "user-data", "user-data-2.0.0", "capture-profile", "media"}
            or parts[-1] in {"nr-assets-path.txt", "neuralhub.ini"}
            or (Path(lower).suffix not in NR_MODEL_SUFFIXES and parts[-1] != "sha256sums")):
        raise ValueError(f"private or unapproved NR model path: {name}")


def validate_nr_models(audit, rows):
    model_rows = {r["path"].lower(): r for r in rows if r["path"].lower().startswith(NR_MODEL_PREFIX)}
    declaration = audit.get("optionalComponents", {}).get("nrModels")
    if declaration is None or declaration == {"bundled": False}:
        return {}  # Existing private-path rejection remains in force below.
    if not isinstance(declaration, dict) or declaration.get("bundled") is not True:
        raise ValueError("invalid optional NR model bundle declaration")
    if declaration.get("sourceArchive") != {"url": NR_MODEL_ARCHIVE_URL, "sha256": NR_MODEL_ARCHIVE_SHA256}:
        raise ValueError("NR models require the exact official HTTPS archive and SHA256 provenance")
    files = declaration.get("files")
    if not isinstance(files, list) or not 0 < len(files) <= 30000:
        raise ValueError("NR models require an explicit nonempty file manifest")
    declared = {}
    for row in files:
        if (not isinstance(row, dict) or set(row) != {"path", "size", "sha256"}
                or type(row["size"]) is not int or row["size"] < 0
                or not isinstance(row["sha256"], str) or not SHA.fullmatch(row["sha256"])):
            raise ValueError("invalid NR model manifest identity")
        check_nr_model_path(row["path"])
        key = row["path"].lower()
        if key in declared:
            raise ValueError("duplicate NR model manifest path")
        declared[key] = row
    if set(declared) != set(model_rows):
        raise ValueError("NR model manifest files differ from audited model rows")
    provenance = {"kind": "official-archive-byte-match", "sourceArchiveSha256": NR_MODEL_ARCHIVE_SHA256}
    for key, identity in declared.items():
        row = model_rows[key]
        if any(row.get(field) != identity[field] for field in ("path", "size", "sha256")):
            raise ValueError("NR model manifest identity differs from audit")
        expected = provenance
        if key == NR_STYLE_CONFIG_PATH:
            if row["sha256"] == NR_STYLE_ZERO_SHA256:
                expected = dict(kind="official-archive-derived-config", sourceArchiveSha256=NR_MODEL_ARCHIVE_SHA256,
                                originalSha256=NR_STYLE_ORIGINAL_SHA256, modified=True,
                                transformation="DLSS5_STYLE=1 -> DLSS5_STYLE=0")
            elif row["sha256"] != NR_STYLE_ORIGINAL_SHA256:
                raise ValueError("NR model style config differs from both pinned configurations")
        if row.get("provenance") != expected:
            raise ValueError("NR model row requires explicit official archive byte provenance or its pinned style derivation")
    return declared


def trust_policy(trust, review, manual_prerelease=False):
    if review and manual_prerelease:
        raise ValueError("review and manual-prerelease modes are mutually exclusive")
    if trust.get("radeonFork") is not True or trust.get("testBuild") is not False:
        raise ValueError("use a Radeon fork build, not an upstream or loopback build")
    repo = trust.get("repository", "")
    if repo:
        repository(repo)
    key = trust.get("publicKey", "")
    if key and not SHA.fullmatch(key):
        raise ValueError("invalid embedded Ed25519 public key")
    if manual_prerelease:
        if repo or key or trust.get("installEnabled") is not False:
            raise ValueError("manual prereleases require an unconfigured, disabled updater")
    elif not review and (not repo or not SHA.fullmatch(key) or trust.get("installEnabled") is not True):
        raise ValueError("public releases require a dedicated repository, key and enabled fork updater")
    return trust


def validate_optional_components(audit, names):
    variant = audit.get("packageVariant", "full")
    components = audit.get("optionalComponents", {})
    if variant not in {"full", "core"} or not isinstance(components, dict):
        raise ValueError("invalid package variant or optional component declaration")
    fidelityfx = components.get("fidelityfx")
    present = set(names) & set(ALL_FSR_RUNTIME_FILES)
    if variant == "core":
        if (not isinstance(fidelityfx, dict) or fidelityfx.get("bundled") is not False
                or not isinstance(fidelityfx.get("reason"), str)
                or not 0 < len(fidelityfx["reason"].strip()) <= 500):
            raise ValueError("Core requires an explicit FidelityFX omission and reason")
        if present:
            raise ValueError("Core must omit all four FidelityFX DLL paths; partial or bundled runtimes are not allowed")
    else:
        if fidelityfx is not None and (not isinstance(fidelityfx, dict) or fidelityfx.get("bundled") is not True):
            raise ValueError("FidelityFX omission requires the explicit Core package variant")
        implementation = fidelityfx.get("implementation", "sdk-modular") if fidelityfx else "sdk-modular"
        if implementation == "open-fsr3":
            if OPEN_FSR_RUNTIME_FILE not in present:
                raise ValueError("missing audited stage files: open-fsr3 requires its source-built FidelityFX DLL")
            if present != {OPEN_FSR_RUNTIME_FILE}:
                raise ValueError("open-fsr3 must include only its source-built FidelityFX DLL; mixed runtimes are not allowed")
            provenance = fidelityfx.get("sha256")
            if not isinstance(provenance, str) or not SHA.fullmatch(provenance):
                raise ValueError("open-fsr3 requires explicit source-built FidelityFX SHA256 provenance")
        elif implementation == "sdk-modular":
            if OPEN_FSR_RUNTIME_FILE in present:
                raise ValueError("the source-built FidelityFX DLL requires the explicit open-fsr3 implementation; mixed runtimes are not allowed")
            if present != set(FSR_RUNTIME_FILES):
                raise ValueError("missing audited stage files: all three FidelityFX DLLs are required for full packages")
        else:
            raise ValueError("unsupported FidelityFX implementation")


def validate_audit(audit, version, source, review):
    if (audit.get("product") != PRODUCT or audit.get("gpuPackage") != "AMD"
            or audit.get("version", "").split("-", 1)[0] != version):
        raise ValueError("Radeon stage identity/version mismatch")
    if not review and (audit.get("releaseReady") is not True or audit.get("localOnly") is not False
                       or audit.get("radeonRedistributionReviewed") is not True):
        raise ValueError("Radeon file-by-file redistribution review is required")
    identity = audit.get("correspondingSource", {})
    if not review or source:
        if (source is None or not source.is_file() or identity.get("asset") != source.name
                or identity.get("sha256") != digest(source)):
            raise ValueError("exact corresponding-source archive is required")
    rows = audit.get("files", [])
    names = [row["path"].lower() for row in rows]
    required = {PLAYER, "qt6core.dll", "platforms/qwindows.dll", "qml/veyra/main.qml", "license", "third_party_notices.md"}
    validate_optional_components(audit, names)
    if len(set(names)) != len(rows) or not required <= set(names) or WORKER in names or "package-manifest.json" in names:
        raise ValueError("duplicate, reserved or missing audited stage files")
    nr_models = validate_nr_models(audit, rows)
    for row in rows:
        check_path(row["path"], nr_models)
        if not SHA.fullmatch(row.get("sha256", "")) or not isinstance(row.get("size"), int) or row["size"] < 0:
            raise ValueError("invalid file hash/size")
        if row["path"].lower() == PROVIDER and audit.get("radeonSourceBuiltProviderSha256") != row["sha256"]:
            raise ValueError("the nvngx-named provider needs explicit source-built Radeon provenance")
        if (row["path"].lower() == OPEN_FSR_RUNTIME_FILE
                and audit["optionalComponents"]["fidelityfx"]["sha256"] != row["sha256"]):
            raise ValueError("source-built FidelityFX provenance does not match the audited DLL hash")
        rights = row.get("redistribution", {})
        if not review and row["path"].lower() not in nr_models and (rights.get("reviewed") is not True or not rights.get("license") or not rights.get("origin")):
            raise ValueError(f"file has no recorded redistribution review: {row['path']}")
    built = audit.get("buildArtifacts", {})
    if set(built) != {"Veyra.exe", WORKER} or any(not SHA.fullmatch(item.get("sha256", ""))
            or not isinstance(item.get("size"), int) or item["size"] <= 0 for item in built.values()):
        raise ValueError("exact launcher and update-worker build identities are required")
    return rows


@contextmanager
def new_artifact(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        raise FileExistsError(path)
    with tempfile.TemporaryDirectory(prefix="radeon-pack-", dir=path.parent) as directory:
        temporary = Path(directory) / path.name
        yield temporary
        if path.exists():
            raise FileExistsError(path)
        temporary.rename(path)


def stem(version):
    return f"Veyra-Radeon-{canonical_version(version)}-AMD-win64-portable"


def wrap(stage, build, output, version, source=None, review=False, manual_prerelease=False):
    version = canonical_version(version)
    stage, build = stage.resolve(strict=True), build.resolve(strict=True)
    audit = json.loads((stage / "package-manifest.json").read_text(encoding="utf-8-sig"))
    trust = trust_policy(json.loads((build / "portable-update-trust.json").read_bytes()), review, manual_prerelease)
    rows = validate_audit(audit, version, source, review)
    if digest(stage / PLAYER) != digest(build / PLAYER):
        raise ValueError("stage EXE differs from build EXE; donor manifests cannot authorize custom builds")
    for name, identity in audit["buildArtifacts"].items():
        path = build / ("veyra_portable_launcher.exe" if name == "Veyra.exe" else name)
        if path.stat().st_size != identity["size"] or digest(path) != identity["sha256"]:
            raise ValueError(f"build artifact differs from audit: {name}")
    for row in rows:
        path = stage / row["path"]
        if (path.resolve(strict=True) != path or not path.is_file()
                or path.stat().st_size != row["size"] or digest(path) != row["sha256"]):
            raise ValueError(f"stage file differs from audit: {row['path']}")
    prefix = stem(version)
    destination = output / f"{prefix}.zip"
    with new_artifact(destination) as temporary, zipfile.ZipFile(temporary, "x", zipfile.ZIP_DEFLATED, compresslevel=3, allowZip64=False) as bundle:
        bundle.writestr(prefix + "/portable-package.txt", MARKER)
        bundle.writestr(prefix + "/current-version.txt", f"{version}\n\n")
        bundle.writestr(prefix + "/portable-update-trust.json", encode(trust))
        bundle.write(build / "veyra_portable_launcher.exe", prefix + "/Veyra.exe")
        folder = prefix + "/versions/" + version + "/"
        bundle.write(build / WORKER, folder + WORKER)
        bundle.writestr(folder + "package-manifest.json", encode(audit))
        for row in rows:
            bundle.write(stage / row["path"], folder + row["path"])
        bundled_nr = audit.get("optionalComponents", {}).get("nrModels", {}).get("bundled") is True
        nr_notice = ("Bundled NR model files are found automatically inside versions/" + version + "/runtime/amd-nr/assets.\n"
                     "Settings remain in runtime_local outside versions.\n" if bundled_nr else
                     "Settings and locally supplied NR assets remain in runtime_local outside versions.\n")
        bundle.writestr(prefix + "/START-HERE.txt", "Veyra for Radeon — unofficial community fork.\n"
                        "Start Veyra.exe. Install only reviewed Radeon releases, never upstream Veyra over this folder.\n"
                        + nr_notice + "To return to the retained previous version: Veyra.exe --rollback\n"
                        "Based on Likely7/Veyra-NRVideo. Not an official Veyra or AMD release.\n")
        if audit.get("packageVariant") == "core":
            bundle.writestr(prefix + "/CORE-PACKAGE.txt", CORE_NOTICE)
        if manual_prerelease:
            bundle.writestr(prefix + "/MANUAL-PRERELEASE.txt", MANUAL_NOTICE)
        if review:
            bundle.writestr(prefix + "/NOT-FOR-PUBLIC-RELEASE.txt", "Local review only. This package has not passed public release checks.\n")
    inspect_bundle(destination, version, source, review=review, manual_prerelease=manual_prerelease)
    return destination


def inspect_bundle(path, version, source=None, review=False, expected_repo=None, expected_key=None, manual_prerelease=False):
    prefix = stem(version)
    if path.name != prefix + ".zip":
        raise ValueError("wrong Radeon ZIP filename")
    folder = prefix + "/versions/" + version + "/"
    with zipfile.ZipFile(path) as bundle:
        entries = bundle.infolist()
        names = [item.filename for item in entries]
        if (not 0 < len(entries) <= 30000 or len(set(n.lower() for n in names)) != len(names)
                or any(not safe_path(item.filename) or not item.filename.startswith(prefix + "/")
                       or item.flag_bits & 1 or stat.S_IFMT(item.external_attr >> 16) not in {0, stat.S_IFREG} for item in entries)):
            raise ValueError("unsafe, duplicate or foreign ZIP entries")
        def small(name, limit):
            if bundle.getinfo(name).file_size > limit:
                raise ValueError("oversized package metadata")
            return bundle.read(name)
        trust = trust_policy(json.loads(small(prefix + "/portable-update-trust.json", 65536)), review, manual_prerelease)
        if expected_repo is not None and trust["repository"] != repository(expected_repo):
            raise ValueError("package repository differs from signing repository")
        if expected_key is not None and trust["publicKey"] != expected_key:
            raise ValueError("signing key does not match embedded update key")
        audit = json.loads(small(folder + "package-manifest.json", 8 * 1024 * 1024))
        rows = validate_audit(audit, version, source, review)
        base = {prefix + "/" + n for n in ("Veyra.exe", "START-HERE.txt", "portable-package.txt", "current-version.txt", "portable-update-trust.json")}
        if review:
            base.add(prefix + "/NOT-FOR-PUBLIC-RELEASE.txt")
        if manual_prerelease:
            base.add(prefix + "/MANUAL-PRERELEASE.txt")
        if audit.get("packageVariant") == "core":
            base.add(prefix + "/CORE-PACKAGE.txt")
        if set(names) != base | {folder + "package-manifest.json", folder + WORKER} | {folder + row["path"] for row in rows}:
            raise ValueError("unreviewed or missing ZIP content")
        if audit.get("packageVariant") == "core" and small(prefix + "/CORE-PACKAGE.txt", 1024) != CORE_NOTICE:
            raise ValueError("invalid Core package notice")
        if manual_prerelease and small(prefix + "/MANUAL-PRERELEASE.txt", 1024) != MANUAL_NOTICE:
            raise ValueError("invalid manual prerelease notice")
        if small(prefix + "/portable-package.txt", 128) != MARKER or small(prefix + "/current-version.txt", 128) != f"{version}\n\n".encode():
            raise ValueError("foreign package marker or version state")
        expanded = sum(item.file_size for item in entries)
        size = path.stat().st_size
        if not 0 < size <= 3 << 30 or not 0 < expanded <= 8 << 30:
            raise ValueError("archive exceeds update limits")
        for row in rows:
            with bundle.open(folder + row["path"]) as stream:
                if bundle.getinfo(folder + row["path"]).file_size != row["size"] or hashlib.file_digest(stream, "sha256").hexdigest() != row["sha256"]:
                    raise ValueError(f"archived bytes differ from audit: {row['path']}")
        for name, identity in audit["buildArtifacts"].items():
            entry = prefix + "/Veyra.exe" if name == "Veyra.exe" else folder + name
            with bundle.open(entry) as stream:
                if bundle.getinfo(entry).file_size != identity["size"] or hashlib.file_digest(stream, "sha256").hexdigest() != identity["sha256"]:
                    raise ValueError(f"archived build artifact differs from audit: {name}")
    return dict(flavor="AMD", asset=path.name, sha256=digest(path), size=size, expandedSize=expanded)


def sign(bundle, source, output, version, repo, key, openssl, notes=""):
    repo = repository(repo)
    if len(notes) > 480:
        raise ValueError("release notes must be at most 480 characters")
    openssl, key = str(openssl.resolve(strict=True)), str(key.resolve(strict=True))
    public = subprocess.check_output([openssl, "pkey", "-in", key, "-pubout", "-outform", "DER"], timeout=120)
    if len(public) != 44 or public[:12].hex() != "302a300506032b6570032100":
        raise ValueError("expected an Ed25519 signing key")
    row = inspect_bundle(bundle, version, source, expected_repo=repo, expected_key=public[12:].hex())
    payload = encode(dict(schema=1, product=PRODUCT, channel=CHANNEL, repository=repo,
                          arch="windows-x64", version=version, notes=notes, packages=[row],
                          correspondingSource={"asset": source.name, "sha256": digest(source)}))
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="radeon-sign-", dir=output) as scratch:
        message = Path(scratch) / "payload.json"
        message.write_bytes(payload)
        signature = subprocess.check_output([openssl, "pkeyutl", "-sign", "-rawin", "-inkey", key, "-in", str(message)], timeout=120)
    if len(signature) != 64:
        raise ValueError("invalid Ed25519 signature length")
    path = output / INDEX
    with new_artifact(path) as temporary:
        temporary.write_bytes(encode({"payload": base64.b64encode(payload).decode("ascii"),
                                      "signature": base64.b64encode(signature).decode("ascii")}))
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    for mode in ("wrap", "inspect", "sign"):
        command = modes.add_parser(mode)
        command.add_argument("--version", required=True)
        command.add_argument("--source", type=Path, required=mode == "sign")
        if mode != "sign":
            policy = command.add_mutually_exclusive_group()
            policy.add_argument("--review", action="store_true")
            policy.add_argument("--manual-prerelease", action="store_true",
                                help="public audited prerelease with the updater unconfigured and disabled")
        if mode != "inspect":
            command.add_argument("--output", type=Path, required=True)
    for name in ("stage", "build"):
        modes.choices["wrap"].add_argument("--" + name, type=Path, required=True)
    for mode in ("inspect", "sign"):
        modes.choices[mode].add_argument("--bundle", type=Path, required=True)
    for name in ("key", "openssl"):
        modes.choices["sign"].add_argument("--" + name, type=Path, required=True)
    modes.choices["sign"].add_argument("--repository", required=True)
    modes.choices["sign"].add_argument("--notes", type=Path)
    args = vars(parser.parse_args())
    mode = args.pop("mode")
    if mode == "sign":
        args["repo"] = args.pop("repository")
        args["notes"] = args["notes"].read_text(encoding="utf-8-sig").strip() if args["notes"] else ""
    elif mode == "inspect":
        args["path"] = args.pop("bundle")
    print({"wrap": wrap, "inspect": inspect_bundle, "sign": sign}[mode](**args))


if __name__ == "__main__":
    main()
