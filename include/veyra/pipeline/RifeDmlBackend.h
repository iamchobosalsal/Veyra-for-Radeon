// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <d3d12.h>
#include <filesystem>
#include <memory>

namespace veyra::pipeline {
// Video-only interpolation: learned bidirectional motion and visibility.
// Records all inference into the caller's D3D12 list. No CPU frame transfers,
// private execution queue, driver hooks, NeuralHub or NVIDIA runtime.
class RifeDmlBackend {
public:
  RifeDmlBackend();
  ~RifeDmlBackend();
  RifeDmlBackend(const RifeDmlBackend &) = delete;
  RifeDmlBackend &operator=(const RifeDmlBackend &) = delete;
  // root contains DirectML.dll and an unmodified RIFE 4.6 or 4.22 Lite
  // NCNN model export. Both are parsed into native DirectML graphs.
  // The initialization list must finish before the first record call executes.
  void open(ID3D12Device *, ID3D12GraphicsCommandList *,
            const std::filesystem::path &root, unsigned width, unsigned height,
            float inferenceScale = 1.f);
  // Images must have matching sizes, input state NSR, output state UAV.
  // RGBA8 is sRGB, RGB10A2 is PQ/BT.2020, RGBA16F is linear scRGB.
  // HDR motion uses a fixed SDR proxy; composition keeps the original HDR.
  // The owner must drain GPU work before destroying this object.
  void record(ID3D12GraphicsCommandList *, ID3D12Resource *previous,
              ID3D12Resource *current, ID3D12Resource *output);
  unsigned graphCount() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace veyra::pipeline
