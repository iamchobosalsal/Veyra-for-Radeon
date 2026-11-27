// SPDX-License-Identifier: GPL-3.0-only
// Independent video FG implementation in Veyra's existing external-provider
// slot. The export names are an interface contract, NOT NVIDIA inference.
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/pipeline/RifeDmlBackend.h"
#include <fstream>
#include <memory>
#include <mutex>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <unordered_map>

extern "C" unsigned rifeGetUnsigned(NVSDK_NGX_Parameter *, const char *,
                                    unsigned *);
extern "C" unsigned rifeGetResource(NVSDK_NGX_Parameter *, const char *,
                                    ID3D12Resource **);
extern "C" void rifeSetUnsigned(NVSDK_NGX_Parameter *, const char *, unsigned);
extern "C" void rifeSetInt(NVSDK_NGX_Parameter *, const char *, int);
namespace {
using namespace veyra::pipeline;
constexpr auto success = NVSDK_NGX_Result_Success;
constexpr auto failure = NVSDK_NGX_Result_FAIL_InvalidParameter;
std::mutex guard;
ComPtr<ID3D12Device> device;
std::filesystem::path root;
void log(const std::string &s) {
  std::ofstream(root / L"rife-video-fg.log", std::ios::app) << s << '\n';
}
void check(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
void checkHr(HRESULT r, const char *message) { check(SUCCEEDED(r), message); }
struct Feature {
  RifeDmlBackend engine;
  ComPtr<ID3D12Resource> previous;
  ComPtr<ID3D12DescriptorHeap> cpuHeap, gpuHeap;
  StateTracker states;
  unsigned w = 0, h = 0, increment = 0, slot = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  bool history = false;
  uint64_t evaluations = 0, resets = 0;
  void flag(ID3D12GraphicsCommandList *list, ID3D12Resource *resource,
            bool disabled) {
    check(resource &&
              resource->GetDesc().Dimension ==
                  D3D12_RESOURCE_DIMENSION_BUFFER &&
              resource->GetDesc().Width >= 4,
          "RIFE requires a validity buffer");
    D3D12_UNORDERED_ACCESS_VIEW_DESC vd{};
    vd.Format = DXGI_FORMAT_R32_TYPELESS;
    vd.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    vd.Buffer.NumElements = 1;
    vd.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    auto cpu = cpuHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateUnorderedAccessView(resource, nullptr, &vd, cpu);
    auto gpu = gpuHeap->GetGPUDescriptorHandleForHeapStart();
    auto dst = gpuHeap->GetCPUDescriptorHandleForHeapStart();
    const unsigned index = (slot++ % 32) * increment;
    gpu.ptr += index;
    dst.ptr += index;
    device->CopyDescriptorsSimple(1, dst, cpu,
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ID3D12DescriptorHeap *heaps[]{gpuHeap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    const unsigned values[]{unsigned(disabled), 0, 0, 0};
    list->ClearUnorderedAccessViewUint(gpu, cpu, resource, values, 0, nullptr);
    states.uavBarrier(list, resource);
  }
};
std::unordered_map<const NVSDK_NGX_Handle *, std::unique_ptr<Feature>> features;
} // namespace
extern "C" NVSDK_NGX_Result NVSDK_CONV
VeyraRifeInit(unsigned long long, const wchar_t *path, ID3D12Device *d,
              NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo *) {
  std::lock_guard lock(guard);
  try {
    check(d && path, "RIFE init arguments");
    check(!device || device.Get() == d, "RIFE device changed without shutdown");
    root = std::filesystem::path(path);
    check(root.is_absolute(), "RIFE absolute runtime path required");
    for (const auto *file : {L"DirectML.dll", L"flownet.param", L"flownet.bin"})
      check(std::filesystem::is_regular_file(root / file),
            "RIFE runtime/model file missing");
    device = d;
    log("RIFE DirectML init; independent video interpolation; no NeuralHub / "
        "NVIDIA runtime");
    return success;
  } catch (const std::exception &e) {
    if (!root.empty())
      log(e.what());
    return failure;
  }
}
extern "C" NVSDK_NGX_Result NVSDK_CONV
VeyraRifePopulate(NVSDK_NGX_Parameter *p) {
  std::lock_guard lock(guard);
  if (!device || !p)
    return failure;
  rifeSetUnsigned(p, "DLSSG.MultiFrameCountMax", 1);
  rifeSetInt(p, "FrameGeneration.Available", 1);
  return success;
}
extern "C" NVSDK_NGX_Result NVSDK_CONV
VeyraRifeCreate(ID3D12GraphicsCommandList *list, NVSDK_NGX_Feature kind,
                NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **out) {
  std::lock_guard lock(guard);
  if (out)
    *out = nullptr;
  try {
    check(device && list && p && out &&
              kind == NVSDK_NGX_Feature_FrameGeneration,
          "RIFE create arguments");
    auto f = std::make_unique<Feature>();
    rifeGetUnsigned(p, "Width", &f->w);
    rifeGetUnsigned(p, "Height", &f->h);
    unsigned format = 0;
    rifeGetUnsigned(p, NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, &format);
    check(format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R16G16B16A16_FLOAT,
          "RIFE requires SDR RGBA8, HDR10 PQ or linear scRGB");
    f->format=DXGI_FORMAT(format);
    const float scale = std::min(1.f, 1080.f / float(f->h));
    f->previous = makeTexture(device.Get(), f->w, f->h,
                              f->format, false);
    check(bool(f->previous), "RIFE history allocation");
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    checkHr(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&f->cpuHeap)),
            "RIFE validity descriptors");
    hd.NumDescriptors = 32;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    checkHr(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&f->gpuHeap)),
            "RIFE validity GPU descriptors");
    f->increment = device->GetDescriptorHandleIncrementSize(hd.Type);
    f->engine.open(device.Get(), list, root, f->w, f->h, scale);
    auto handle = reinterpret_cast<NVSDK_NGX_Handle *>(f.get());
    log(std::format("create {}x{} inferenceScale={} stages={} format={} HDR motion uses fixed SDR proxy; composition retains original luminance", f->w, f->h,
                    scale, f->engine.graphCount(), format));
    features.emplace(handle, std::move(f));
    *out = handle;
    return success;
  } catch (const std::exception &e) {
    log(std::string("create failed: ") + e.what());
    return failure;
  }
}
extern "C" NVSDK_NGX_Result NVSDK_CONV VeyraRifeEvaluate(
    ID3D12GraphicsCommandList *list, const NVSDK_NGX_Handle *handle,
    NVSDK_NGX_Parameter *p, PFN_NVSDK_NGX_ProgressCallback) {
  std::lock_guard lock(guard);
  try {
    check(list && p, "RIFE evaluate arguments");
    auto it = features.find(handle);
    check(it != features.end(), "RIFE unknown handle");
    auto &f = *it->second;
    ID3D12Resource *current = nullptr, *output = nullptr, *disabled = nullptr;
    rifeGetResource(p, NVSDK_NGX_DLSSG_Parameter_Backbuffer, &current);
    rifeGetResource(p, NVSDK_NGX_DLSSG_Parameter_OutputInterpolated, &output);
    rifeGetResource(p, NVSDK_NGX_DLSSG_Parameter_OutputDisableInterpolation,
                    &disabled);
    unsigned count = 0, index = 0, reset = 0;
    rifeGetUnsigned(p, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, &count);
    rifeGetUnsigned(p, NVSDK_NGX_DLSSG_Parameter_MultiFrameIndex, &index);
    rifeGetUnsigned(p, NVSDK_NGX_DLSSG_Parameter_Reset, &reset);
    if (!(current && output && count == 1 && index == 1))
      log(std::format(
          "invalid pair current={} output={} count={} index={} reset={}",
          bool(current), bool(output), count, index, reset));
    check(current && output && count == 1 && index == 1,
          "RIFE supports one generated frame per pair");
    for (auto r : {current, output}) {
      auto d = r->GetDesc();
      check(d.Width == f.w && d.Height == f.h &&
                d.Format == f.format,
            "RIFE frame extent/format changed; recreate feature");
    }
    f.states.set(current, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    f.states.set(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const bool warmup = reset || !f.history;
    if (warmup) {
      f.states.transition(list, current, D3D12_RESOURCE_STATE_COPY_SOURCE);
      f.states.transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST);
      list->CopyResource(output, current);
      f.states.transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      ++f.resets;
    } else
      f.engine.record(list, f.previous.Get(), current, output);
    // The host's create-time warmup intentionally omits this optional
    // buffer. Real presentation supplies it and honours disabled warmups.
    if (disabled)
      f.flag(list, disabled, warmup);
    f.states.transition(list, current, D3D12_RESOURCE_STATE_COPY_SOURCE);
    f.states.transition(list, f.previous.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(f.previous.Get(), current);
    f.states.transition(list, f.previous.Get(),
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    f.states.transition(list, current,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    f.history = true;
    ++f.evaluations;
    if (f.evaluations == 1 || f.evaluations % 120 == 0)
      log(std::format("evaluated={} resetWarmups={} validThisPair={}",
                      f.evaluations, f.resets, !warmup));
    return success;
  } catch (const std::exception &e) {
    log(std::string("evaluate failed: ") + e.what());
    return failure;
  }
}
extern "C" NVSDK_NGX_Result NVSDK_CONV VeyraRifeRelease(NVSDK_NGX_Handle *h) {
  std::lock_guard lock(guard);
  return features.erase(h) == 1 ? success : failure;
}
extern "C" NVSDK_NGX_Result NVSDK_CONV VeyraRifeShutdown(ID3D12Device *d) {
  std::lock_guard lock(guard);
  if (device.Get() != d || !features.empty())
    return failure;
  log("shutdown after feature drain");
  device.Reset();
  return success;
}
