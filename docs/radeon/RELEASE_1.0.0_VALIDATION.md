# Radeon 1.0.0 validation record

Date: 2026-11-27. GPU: AMD Radeon RX 9070 XT, Windows x64.

## Failure corrected

The previous public package included the NR host DLLs but excluded private weights. The UI checked the runtime more loosely than the backend, allowing NR selection followed by activation failure. UI and backend now share mode-specific asset validation: tensor names/sizes/readability, manifest, codecs and HIP modules.

The folder picker stores a local UTF-8 reference without copying or downloading models. A stale explicit reference fails closed. Missing assets preserve NR-on intent; valid registration retries the failed chain. Replacing assets while NR is already running requires restart. Small 1080p initialization also prewarms Original, so Small requires Original weights too.

## Automated and actual-app checks

| Check | Result | Scope |
|---|---|---|
| NR asset metadata | 318 checks, pass | Synthetic sparse files: missing/truncated tensors, shared resources, stale/malformed references, mode dependencies, discovery and new-layer selection |
| Package rules | 37 tests, pass | Synthetic packaging fixtures, source/hash matching, private-file exclusion and Open FSR contract |
| Effect selection | 7 tests, pass | JavaScript integration and GPU-switch policy |
| NR C ABI | 66 checks, pass | Fake provider, real D3D12 identity copy; not neural inference |
| Real provider metadata | 11 checks, pass | Real Small/Original Create/PrepareSession, vendor/mode/asset rejection, swapped module, teardown/reopen; no HIP inference |
| Related regressions | Pass | HDR tuning, ANR/video-AI/NR settings, effect availability, color-look persistence/import and capture color contract |
| Fresh actual app | Pass | Missing models, invalid-folder rejection, valid-folder registration, color-panel default save and field/group/all reset |
| Stale path recovery | Pass | Restart with missing reference, preserve NR-on intent, reconnect and observe NR active plus actual GPU-stage timing samples |
| Actual app restart | Pass | Retain folder, NR-on state, current color and separately saved default; open video and observe measured NR processing |

Actual-app tests use an isolated profile and disposable QML driver exercising product APIs and color controls. The driver is removed after every run and excluded from the program ZIP. Valid model weights and the test recording remain local.

## Real NR + HDR + FG graph

Production settings, stage description and EnhanceGraph process four frames per case from a local BT.709 recording. Source/output: 3840×2160; internal NR: 1920×1080 Small; strength: 1.0; HDR native tone expansion enabled. History resets at frames 0 and 3.

| NR layers | FG off | RIFE 2× | FSR 2× |
|---|---|---|---|
| 1 | Pass | Pass | Pass |
| 2 | Pass | Pass | Pass |
| 3 | Pass | Pass | Pass |

Totals: **36 real frames, 72 NR layer evaluations, 12 generated frames**. Checks cover processing, dimensions, finite/range/variance samples, full-byte hashes, processed layer outputs, generated identities and reset warm-up. Numeric sampling covers every fourth pixel/row. Inspected diagnostic comparisons showed no blank output or gross corruption.

FSR real frames matched FG off byte-for-byte. RIFE can also change real-frame NR guidance through shared motion refinement, so its real-frame outputs need not match FG off.

This matrix preceded the final metadata-only Small→Original prerequisite correction. The final player was subsequently rebuilt and exercised with the complete bundle in the actual-app phases. Inference/FG algorithms were unchanged by that prerequisite check.

Earlier FSR SDR/scRGB/PQ/reset tests and the MIT runtime build are recorded in [FSR_OPEN_VALIDATION.md](FSR_OPEN_VALIDATION.md). FSR and RIFE are separate alternatives; both implementations are included.

## Artifact checks

Release gates verify the staged inventory/SHA256 values and binary/source ZIP correspondence, then start the exact extracted program in a fresh directory. The original public player hash is recorded below; the NR UI rebuild hash is recorded in the addendum. Profiles, local model references, private NR weights, logs, recordings and credentials are excluded. The seven README screenshots are separately provided public examples, copied unchanged; they are not the private video fixture.

## Limits

- Short regression checks do not prove flicker-free output or long-session stability. Bright/white-region NR+FG flicker may remain.
- Layer stacking can soften detail. README screenshots use separate captures and unrecorded strengths; they are illustrative rather than a controlled benchmark.
- No physical input-latency measurement or universal Radeon/driver/capture-device compatibility claim is made.
- Diagnostic process durations include initialization/readback; they are not per-frame latency or sustainable FPS.
- NR weights remain separately required. Metadata readiness does not guarantee arbitrary supplied weights will execute correctly.

## Reproduction sources

The corresponding source includes `tests/radeon/NrModelAssetsTests.cpp`, `NrStackQualityProbe.cpp` / `NrIntegrationProbe.h`, `ReleaseFirstRunSmoke.qml`, `tests/unit/LmxxfNrTests.cpp` and `tests/integration/LmxxfNrRuntimeMetadataTests.cpp`. The app driver receives model/video paths at test time; personal paths are not embedded in public source.

Original 1.0.0 tested player SHA256: `c50d90ca97c5e60606408ded9f72e1681269475768ea3aa7ba14628562c02c72`.

GPU graph probe SHA256: `56909d60ace4b1b056e7cb78eb15a6cd3022b2209f437d083d2d17ccb9b1f680`.

## NR connection UI rebuild — 2026-11-27

This rebuild fixes recovery from missing local models in the main UI and restores absent SR/FG stages when applying the combined preset. GPU processing algorithms and shader binaries are unchanged. The cancelled fullscreen input candidate is excluded.

- Effect-support / GPU-switch regressions: 13 passed.
- Actual app, fresh profile with zero NR layers: recovery action visible; busy state blocked; invalid-folder error recoverable; valid local folder registered; actual preset click enabled Radeon NR, RIFE 2X and 4K.
- Actual app, legacy session with three disabled runtime-3 NR nodes and no SR/FG rows: the same recovery sequence passed; missing SR/FG rows were restored. The pre-fix failure was retained as evidence.
- Native folder picker and live capture: the main action opened the Windows folder picker, an existing local model folder was selected, and the ready preset enabled real NR processing and RIFE generation. The fresh-profile drivers themselves use no source and do not prove inference.

UI rebuild player SHA256: `86d6f024730618694bc6a01d7aadfa91f16a7a2845f4724c585b4dfb1a905dc7`.

ProPage.qml SHA256: `907d007de9a50c470678dbfa9f044a235c4e370d885c60171eec94ba000860fe`.

EffectSupport.js SHA256: `eece3c0ec198c5f90098a34e0626b1751e709390e6d7d286824e38b3dd19e693`.

The earlier nine-case GPU matrix remains historical coverage; it was not rerun or counted as new UI-rebuild coverage. The public package continues to exclude NR weights, personal model references and user profiles. Test model paths are supplied at runtime, not embedded in public QML.
