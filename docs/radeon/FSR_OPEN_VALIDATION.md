# Open FSR preview validation — 2026-11-27

Target: `1.0.1-radeon-preview.2`, RX 9070 XT, Windows x64. Base application source is public commit `e26c2659ff9931a3d1328b47acd6288a942d3f13`. All runs used isolated directories; no live capture device or personal configuration was used.

## Build and runtime identity

- Application, launcher and updater: fresh MSVC Release build from this source.
- Open SDK runtime: `amd_fidelityfx_dx12.dll`, SHA-256 `784ee2f75109aba17e390fca9119dbfdb68b378e8063cb9cbbaa1dd25057a156`.
- Runtime source: FidelityFX SDK 1.1.4 commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`, MIT. Matching patched source archive and recipe accompany this release.
- Runtime enumeration confirms SR `3.1.4` / `0xF5A5CA1E00C01004` and FG `1.1.3` / `0xF600000000401003`. Driver provider substitution is disabled in this build.
- No signed SDK2.3 provider DLLs or FSR4 ML models were used. No NR/RIFE algorithm change is included in this release.

## Bounded synthetic GPU checks

Build diagnostic target `veyra_fsr_open_probe` explicitly; it is not a CTest auto-run target. Place the packaged runtime at `runtime_local/amd/fidelityfx/amd_fidelityfx_dx12.dll` beside the private probe, then run `selection`, `small`, and `hdr4k` modes separately. Each process was bounded to 60 seconds with a Windows system-only PATH.

- `selection`: seven capability contracts, including complete/partial legacy layouts, open-runtime precedence and disabled ML.
- `small`: SDR SR 640×360→1280×720; standalone FG 1280×720; combined SR+FG 960×540→1920×1080 after context re-creation.
- `hdr4k`: FP16 scRGB SR 1920×1080→3840×2160 + FG; native 3840×2160 R10G10B10A2 PQ FG.
- Six inputs per case, history resets at frames 0 and 3. Generated frames are checked against midpoint position/width, so holding an original frame or creating a broad double image cannot pass merely because dispatch returned success.
- HDR checks retain values above SDR white (scRGB peak 6.25), valid PQ values and finite pixel data. They do not validate monitor luminance or artistic tone mapping.
- The first small probe run found a test counter mistake: SR counters persist across context re-creation. The test now checks the per-case delta. Production counters and rendering were not changed to satisfy that test.

## UI and RIFE checks

The rebuilt application completes its hidden `--portable-update-probe 1.0.1` startup with a private profile and exits normally. A disposable QML instrument in a separate copy reads the actual bridge capability map: FSR SR (`sr5`), FSR3 FG and RIFE are available; FSR4 ML is unavailable. The instrument is not part of the release.

The unchanged source-built RIFE provider SHA-256 is `94907a7c34928970608dbf4f9b7b38b13c474864a466ac2d1ffbaac6adccf6c4`. A clean process using only the four RIFE files extracted from the preceding public ZIP produced three nontrivial 640×360 interpolated frames from four inputs. This is a component check, not a new 4K RIFE performance result.

## Packaging and limits

The package test suite covers the exact monolith path/hash, mixed-runtime rejection, complete source and file audits, and disabled-updater manual prereleases. The old Core and modular-runtime profiles retain their original guards. Final ZIP inspection compares every file hash with its audit and includes exact corresponding source and dependency recipes.

These tests do **not** measure controller-to-photon latency, capture-card delay, sustained 4K frame rate, or a live NR+FG game's flicker. Bright-area flicker and cumulative NR softness remain documented limitations. No claim of FSR4 ML support, NVIDIA-equivalent quality or universal Radeon compatibility is made.
