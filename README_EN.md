# Veyra for Radeon

An unofficial Veyra fork with additional tuning and optimization of image processing, stability, and runtime packaging for Radeon GPUs.

**Upstream Veyra already supports AMD/Radeon and provides [AMD installers and portable builds](https://github.com/Likely7/Veyra-NRVideo/releases/tag/v2.2.1).** This fork builds on that support, adjusts multi-layer NR processing and its interaction with HDR and frame generation, and organizes the Radeon runtime package and local model connection. The underlying application and existing features, including multi-layer NR, originate from upstream Veyra and its contributors.

Image quality and performance depend on the GPU, settings, model, and source. The term optimization describes this fork's focus; the available validation does not establish an across-the-board advantage over upstream.

This is not an official release of Veyra, AMD, or NVIDIA. Original credits and licenses are preserved.

[Package contents, setup, limitations, and credits (Korean)](README-RADEON.ko.md) · [Download this fork](https://github.com/iamchobosalsal/Veyra-for-Radeon/releases/tag/v1.0.0) · [Upstream Veyra](https://github.com/Likely7/Veyra-NRVideo)

## Bundled NR model fix — 2026-11-27

The updated 1.0.0 portable ZIP includes the Small and Original NR models and shared runtime assets omitted from the earlier small download. Extract the entire ZIP into a new folder, start Veyra.exe, and apply **Radeon NR + RIFE** after the automatic model check. An existing Veyra installation or manual model folder connection is no longer required for a fresh installation.

The model files are preserved from the upstream Veyra 2.2.0 AMD public package. Upstream notices and file identities are retained; the NVIDIA model weights are not covered by lmxxf's MIT license or relicensed by this fork. See [NR asset provenance](docs/radeon/NR_BUNDLE_PROVENANCE.md).
