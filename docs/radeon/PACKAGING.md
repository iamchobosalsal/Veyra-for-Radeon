# Radeon release preparation

The helper does not publish, generate signing keys, modify an installation, or establish redistribution rights. Its checks prevent accidentally signing a donor package, upstream update, or personal runtime directory.

## Build inputs

`build/portable-update-trust.json` must identify this fork:

```json
{"radeonFork":true,"repository":"OWNER/REPO","publicKey":"64 lowercase hex characters","installEnabled":true,"testBuild":false}
```

Use a dedicated repository and signing key. `Likely7/Veyra-NRVideo` is rejected. Blank repository/key are accepted with `--review` for private review or with `--manual-prerelease` for a fully audited public manual prerelease. Manual prereleases require `installEnabled: false`, `radeonFork: true` and `testBuild: false`. Neither mode can be signed as an installable update. The two modes are mutually exclusive.

The build directory must contain `veyra_qml_ui.exe`, `veyra_portable_launcher.exe`, and `veyra_update_worker.exe` from the reviewed Radeon build.

## Audited stage

Supply `stage/package-manifest.json` with:

- `product: "Veyra-Radeon-portable"`, `gpuPackage: "AMD"`, and matching numeric `version` (a display suffix is allowed).
- For public releases: `releaseReady: true`, `localOnly: false`, and `radeonRedistributionReviewed: true`, set only after the actual review.
- `correspondingSource: {"asset":"source-archive.zip","sha256":"..."}` identifying the exact source archive supplied with `--source` and published alongside the binary.
- `buildArtifacts`: keys `Veyra.exe` and `veyra_update_worker.exe`, each with `size` and `sha256` of the launcher and worker from this build.
- `files`: the complete stage inventory, each row with `path`, `size`, `sha256`. Public rows additionally need `redistribution: {"reviewed":true,"license":"...","origin":"..."}`. These records document the maintainer's review; they are not automatic legal approval.
- When the independently built RIFE provider uses `runtime/experimental/anr/nvngx_dlssg.dll`, `radeonSourceBuiltProviderSha256` must match that file's audited hash. The filename alone is not evidence of NVIDIA ownership.

Required inventoried files include the player, Qt6Core, qwindows, Main.qml, LICENSE and THIRD_PARTY_NOTICES. The helper adds the worker, launcher, state, trust metadata and start instructions. Do not put the worker or `package-manifest.json` in `files`.

Private profiles/logs, the installed root runtime_local directory, startup music, private NR assets, NVIDIA runtime folders, bootstrap proxies, driver DLLs and keys are excluded by policy. The only version-local runtime_local exceptions are the required FidelityFX loader, upscaler and framegeneration SDK DLLs under runtime_local/amd/fidelityfx; these are program dependencies, not personal settings. A full package must include all three. An explicitly declared Core package must omit all three; a partial set is rejected. Keep locally supplied NR assets under the installed root's `runtime_local/radeon/nr-assets`; do not include them in an update ZIP.

## Public manual Core prerelease

Public release is separate from enabling the automatic updater. The current Core prerelease leaves the repository/key blank and update installation disabled. Use `--manual-prerelease` for both wrapping and inspecting. This preserves the public requirements for an exact corresponding-source archive, complete file inventory and per-file redistribution review; it does not waive them. It writes `MANUAL-PRERELEASE.txt`, not the private `NOT-FOR-PUBLIC-RELEASE.txt`, and cannot produce a signed update index.

To omit the optional FidelityFX runtime, the audited manifest must include:

```json
{
  "packageVariant": "core",
  "optionalComponents": {
    "fidelityfx": {
      "bundled": false,
      "reason": "FidelityFX binary runtime redistribution is not cleared for this combined package; FSR is unavailable."
    }
  }
}
```

All three `FSR_RUNTIME_FILES` must be absent from the manifest and ZIP. The helper adds and checks `CORE-PACKAGE.txt`, which states that FSR super resolution and FSR frame generation are unavailable. Native 4K capture/output is separate from FSR. A normal full package continues to require the entire trio. This changes packaging only, not the source feature switches or anyone's installed application. Do not claim bundled NR weights or universally tested hardware support.

```powershell
python scripts/radeon/package_release.py wrap --stage STAGE_CORE --build MATCHED_BUILD --version 1.0.0 --source SOURCE.zip --output OUTPUT --manual-prerelease
python scripts/radeon/package_release.py inspect --bundle OUTPUT/Veyra-Radeon-1.0.0-AMD-win64-portable.zip --version 1.0.0 --source SOURCE.zip --manual-prerelease
```

Publish such an artifact as a GitHub **pre-release**, clearly labeled Core/experimental. Numeric package layout remains `1.0.0`; a display/tag suffix such as `1.0.0-radeon-preview` does not enable the stable updater. Later public manual releases must be installed separately after reviewing their notes.

## Signed full-release commands

```powershell
python scripts/radeon/package_release.py wrap --stage STAGE --build BUILD --version 1.0.0 --source SOURCE.zip --output OUTPUT
python scripts/radeon/package_release.py inspect --bundle OUTPUT/Veyra-Radeon-1.0.0-AMD-win64-portable.zip --version 1.0.0 --source SOURCE.zip
python scripts/radeon/package_release.py sign --bundle OUTPUT/Veyra-Radeon-1.0.0-AMD-win64-portable.zip --version 1.0.0 --source SOURCE.zip --repository OWNER/REPO --key PRIVATE-KEY.pem --openssl OPENSSL.exe --output OUTPUT
```

Only the explicit `sign` command produces `Veyra-Radeon-portable-update-v1.json`. It checks the key against the embedded build key, binds the signed payload to the exact fork repository/product/channel, and refuses review packages. Keep the private key outside the source repository and packages.

For a local packaging review with an unconfigured feed, append `--review` to `wrap` and `inspect`. Such a ZIP contains `NOT-FOR-PUBLIC-RELEASE.txt`; it is not a public release or approved update.

No files are uploaded. For the signed full-release path, the maintainer reviews the ZIP/source/index and then explicitly publishes a stable `v1.0.0` release to the fork repository. Public manual prereleases instead follow the Core/manual procedure above and do not create an index. Later Radeon releases advance this independent numeric version. Original Veyra releases must be merged, rebuilt and validated before producing a Radeon release.

## Validation

`python tests/radeon/test_package_release.py -q` exercises isolated synthetic fixtures. It verifies private-file exclusion, stale source/EXE/shader/worker rejection, ZIP tamper detection, repository/key binding, distinct fork marker, review-only refusal, source-built RIFE identity, non-overwriting output behavior, explicit manual-prerelease trust, retained public audit checks, Core all-or-none FSR omission, package notices, and refusal to sign manual prereleases as updates. It does not validate real video processing or license clearance.
