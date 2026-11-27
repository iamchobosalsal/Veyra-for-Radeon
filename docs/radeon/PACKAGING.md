# Radeon release preparation

Current target: `v1.0.1-radeon-preview.2`, numeric package version `1.0.1`, based on Radeon fork commit `e26c2659ff9931a3d1328b47acd6288a942d3f13`. This manual prerelease includes both the open FSR 3 implementation and RIFE 2X. They are alternative selectable FG backends, not stacked generators. NR weights and FSR 4 ML components remain excluded.

The helper does not publish, generate signing keys, modify an installation, or establish redistribution rights. Its checks prevent accidentally signing a donor package, upstream update, or personal runtime directory.

## Build inputs

`build/portable-update-trust.json` must identify this fork:

```json
{"radeonFork":true,"repository":"","publicKey":"","installEnabled":false,"testBuild":false}
```

This is the current manual-prerelease trust configuration. A future signed update needs a dedicated repository and signing key; `Likely7/Veyra-NRVideo` is rejected. Blank repository/key are accepted with `--review` for private review or with `--manual-prerelease` for a fully audited public manual prerelease. Manual prereleases require `installEnabled: false`, `radeonFork: true` and `testBuild: false`. Neither mode can be signed as an installable update. The two modes are mutually exclusive.

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

Private profiles/logs, the installed root runtime_local directory, startup music, private NR assets, NVIDIA runtime folders, bootstrap proxies, driver DLLs and keys are excluded by policy. The version-local `runtime_local/amd/fidelityfx` exception is limited to the declared FidelityFX implementation. This release uses the single source-built `amd_fidelityfx_dx12.dll` and excludes all three SDK 2.3.0 signed DLLs. The legacy full implementation requires its complete signed trio, while Core must omit all four possible FSR runtime files. Mixing implementations or including a partial signed trio is rejected. Keep locally supplied NR assets under the installed root's `runtime_local/radeon/nr-assets`; do not include them in an update ZIP.

## Public manual Open FSR prerelease

Public release is separate from enabling the automatic updater. Leave the
repository/key blank and update installation disabled; automatic checking,
download and installation remain off. Use `--manual-prerelease` for both wrapping
and inspecting. This retains the exact corresponding-source, complete inventory
and per-file redistribution review requirements. It writes
`MANUAL-PRERELEASE.txt`, not `NOT-FOR-PUBLIC-RELEASE.txt`, and cannot produce a
signed update index.

Declare the verified open runtime explicitly:

```json
{
  "packageVariant": "full",
  "optionalComponents": {
    "fidelityfx": {
      "bundled": true,
      "implementation": "open-fsr3",
      "sha256": "784ee2f75109aba17e390fca9119dbfdb68b378e8063cb9cbbaa1dd25057a156"
    }
  }
}
```

The hash must match the inventory row and actual file at
`runtime_local/amd/fidelityfx/amd_fidelityfx_dx12.dll` (6,829,568 bytes).
Do not include `amd_fidelityfx_loader_dx12.dll`,
`amd_fidelityfx_upscaler_dx12.dll` or `amd_fidelityfx_framegeneration_dx12.dll`.
The MIT runtime is built from SDK 1.1.4 / FSR 3.1.4; its FG provider reports
internal version `1.1.3`. FSR 4 ML is not bundled or claimed.

Include the exact SDK corresponding source archive, its five-file patch and
build recipe described in [BUILD_PUBLIC.md](BUILD_PUBLIC.md), plus the matching
Veyra source and other required dependency sources. The RIFE implementation,
MIT model and source-built provider remain included with their existing audit
records. Preserve all notices; do not include personal NR models or settings.
`ColorApply` remains unchanged from the prior Radeon source baseline.

```powershell
python scripts/radeon/package_release.py wrap --stage STAGE_FULL --build MATCHED_BUILD --version 1.0.1 --source SOURCE.zip --output OUTPUT --manual-prerelease
python scripts/radeon/package_release.py inspect --bundle OUTPUT/Veyra-Radeon-1.0.1-AMD-win64-portable.zip --version 1.0.1 --source SOURCE.zip --manual-prerelease
```

Publish as the GitHub **pre-release** `v1.0.1-radeon-preview.2`. Install separately
from the old Core preview; numeric layout is `versions/1.0.1/`. A display/tag suffix
does not enable the stable updater. Packaging tests and real GPU validation are
separate requirements; no successful integration-test result is implied by this
document or by the completed SDK library build.

The historical Core variant remains explicit: `packageVariant: "core"`,
`optionalComponents.fidelityfx.bundled: false`, a reason explaining that FSR is
unavailable, and none of the four possible FSR runtime files. This is not the
current release variant. Full packages without `implementation: "open-fsr3"`
retain the legacy signed-trio contract; they require their own redistribution review.

## Future signed stable releases (not this prerelease)

The commands below are examples for a separately configured future stable `1.1.0` build. Do not enable or sign an update for the current manual prerelease.

```powershell
python scripts/radeon/package_release.py wrap --stage STAGE --build BUILD --version 1.1.0 --source SOURCE.zip --output OUTPUT
python scripts/radeon/package_release.py inspect --bundle OUTPUT/Veyra-Radeon-1.1.0-AMD-win64-portable.zip --version 1.1.0 --source SOURCE.zip
python scripts/radeon/package_release.py sign --bundle OUTPUT/Veyra-Radeon-1.1.0-AMD-win64-portable.zip --version 1.1.0 --source SOURCE.zip --repository OWNER/REPO --key PRIVATE-KEY.pem --openssl OPENSSL.exe --output OUTPUT
```

Only the explicit `sign` command produces `Veyra-Radeon-portable-update-v1.json`. It checks the key against the embedded build key, binds the signed payload to the exact fork repository/product/channel, and refuses review packages. Keep the private key outside the source repository and packages.

For a local packaging review with an unconfigured feed, append `--review` to `wrap` and `inspect`. Such a ZIP contains `NOT-FOR-PUBLIC-RELEASE.txt`; it is not a public release or approved update.

No files are uploaded. For the signed full-release path, the maintainer reviews the ZIP/source/index and then explicitly publishes a stable `v1.1.0` release to the fork repository. Public manual prereleases instead follow the Open FSR/manual procedure above and do not create an index. Later Radeon releases advance this independent numeric version. Original Veyra releases must be merged, rebuilt and validated before producing a Radeon release.

## Validation

`python tests/radeon/test_package_release.py -q` exercises isolated synthetic fixtures. It verifies private-file exclusion, stale source/EXE/shader/worker rejection, ZIP tamper detection, repository/key binding, distinct fork marker, review-only refusal, source-built RIFE identity, non-overwriting output behavior, explicit manual-prerelease trust, retained public audit checks, Core all-or-none FSR omission and the full Open FSR single-DLL/hash contract, package notices, and refusal to sign manual prereleases as updates. It does not validate real video processing or license clearance.
