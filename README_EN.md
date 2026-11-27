# Veyra for Radeon

An unofficial Veyra fork with additional tuning and optimization of image processing, stability, and runtime packaging for Radeon GPUs.

**Upstream Veyra already supports AMD/Radeon and provides [AMD installers and portable builds](https://github.com/Likely7/Veyra-NRVideo/releases/tag/v2.2.1).** This fork builds on that support, adjusts multi-layer NR processing and its interaction with HDR and frame generation, and organizes the Radeon runtime package and local model connection. The underlying application and existing features, including multi-layer NR, originate from upstream Veyra and its contributors.

Image quality and performance depend on the GPU, settings, model, and source. The term optimization describes this fork's focus; the available validation does not establish an across-the-board advantage over upstream.

This is not an official release of Veyra, AMD, or NVIDIA. Original credits and licenses are preserved.

[Package contents, setup, limitations, and credits (Korean)](README-RADEON.ko.md) · [Download this fork](https://github.com/iamchobosalsal/Veyra-for-Radeon/releases/tag/v1.0.0) · [Upstream Veyra](https://github.com/Likely7/Veyra-NRVideo)

## NR connection UI fix — 2026-11-27

When models are missing, the top action now reads **Connect NR models** and opens the local folder picker. Select an existing model folder, then click **Radeon NR + RIFE** to apply the preset. The NR settings also expose model connection and recheck actions even with no layers. Applying the preset restores missing SR/FG stages while preserving existing NR strengths. Re-download the updated 1.0.0 ZIP if you have the earlier package; model weights are still supplied separately.
