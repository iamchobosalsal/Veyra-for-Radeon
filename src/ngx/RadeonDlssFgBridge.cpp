// SPDX-License-Identifier: GPL-3.0-only
// Veyra 2.2 external-FG ABI adapter. The installed NeuralHub runtime supplies
// DLSS-FG; this DLL only adapts Veyra's existing external-provider contract.
// No provider code or weights are included in this source or DLL.
#include <d3d12.h>
#include <dxgi1_4.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <string>
#include <unordered_map>
#include <windows.h>

namespace {
using Init = NVSDK_NGX_Result(NVSDK_CONV *)(
    unsigned long long, const wchar_t *, ID3D12Device *, NVSDK_NGX_Version,
    const NVSDK_NGX_FeatureCommonInfo *);
using Populate = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Parameter *);
using Create = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D12GraphicsCommandList *,
                                              NVSDK_NGX_Feature,
                                              NVSDK_NGX_Parameter *,
                                              NVSDK_NGX_Handle **);
using Evaluate = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D12GraphicsCommandList *,
                                                const NVSDK_NGX_Handle *,
                                                NVSDK_NGX_Parameter *,
                                                PFN_NVSDK_NGX_ProgressCallback);
using Release = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Handle *);
using Shutdown = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D12Device *);
using Caps = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Parameter **);
using DestroyParams = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Parameter *);
struct Runtime {
  HMODULE bootstrap = nullptr, host = nullptr;
  Init init = nullptr;
  Populate populate = nullptr;
  Create create = nullptr;
  Evaluate evaluate = nullptr;
  Release release = nullptr;
  Shutdown shutdown = nullptr;
  Caps caps = nullptr;
  DestroyParams destroyParams = nullptr;
} runtime;
// The player/runtime use MSVC's overloaded virtual-method layout. This adapter
// is also buildable with MinGW; cross that boundary through NGX's C exports.
PFN_NVSDK_NGX_Parameter_GetUI getUI = nullptr;
PFN_NVSDK_NGX_Parameter_GetI getI = nullptr;
PFN_NVSDK_NGX_Parameter_GetD3d12Resource getResource = nullptr;
PFN_NVSDK_NGX_Parameter_SetUI setUI = nullptr;
PFN_NVSDK_NGX_Parameter_SetI setI = nullptr;
PFN_NVSDK_NGX_Parameter_SetF setF = nullptr;
bool ready = false;
std::mutex mutex;
struct Feature {
  unsigned width = 0, height = 0;
  uint64_t evaluations = 0;
};
std::unordered_map<const NVSDK_NGX_Handle *, Feature> features;
struct BootstrapSurface {
  HWND window = nullptr;
  IDXGIFactory2 *factory = nullptr;
  ID3D12CommandQueue *queue = nullptr;
  IDXGISwapChain1 *swapchain = nullptr;
  ~BootstrapSurface() {
    if (swapchain)
      swapchain->Release();
    if (queue)
      queue->Release();
    if (factory)
      factory->Release();
    if (window)
      DestroyWindow(window);
  }
  bool open(ID3D12Device *device) {
    // The installed game runtime initializes its GPU engine on first
    // Present. Veyra validates FG before creating its video sink. Prime
    // that lifecycle once, on this exact device, using a hidden surface.
    window =
        CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
                        L"Veyra Radeon runtime", WS_POPUP, 0, 0, 16, 16,
                        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window || FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
      return false;
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))))
      return false;
    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Width = 16;
    sc.Height = 16;
    sc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 2;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(factory->CreateSwapChainForHwnd(queue, window, &sc, nullptr,
                                               nullptr, &swapchain)))
      return false;
    return SUCCEEDED(swapchain->Present(0, 0));
  }
};
// NeuralHub installs process-local window/queue hooks and retains its compute
// device across NGX Shutdown1 calls. Keep exactly one bootstrap/device alive
// for that same process lifetime. Releasing/recreating the priming window on
// each graph toggle invalidates the runtime's window-hook chain. The OS owns
// final cleanup, and the tiny surface cannot grow with graph replacements.
BootstrapSurface *bootstrapSurface = nullptr;
ID3D12Device *bootstrapDevice = nullptr;
std::filesystem::path application() {
  wchar_t path[32768]{};
  GetModuleFileNameW(nullptr, path, 32768);
  return std::filesystem::path(path).parent_path();
}
void log(const std::string &message) {
  std::ofstream f(application() / L"radeon-dlss-fg.log", std::ios::app);
  f << message << '\n';
}
bool load() {
  if (ready)
    return true;
  // Keep the game bootstrap out of the executable search directory. NVIDIA
  // uses Veyra's native NGX path and must not auto-load Radeon game hooks.
  const auto loader = application() / L"runtime/experimental/anr/bootstrap/winmm.dll";
  runtime.bootstrap = LoadLibraryExW(loader.c_str(), nullptr,
                                     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                         LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (!runtime.bootstrap) {
    log("bootstrap load failed " + std::to_string(GetLastError()));
    return false;
  }
  using Tick = DWORD(WINAPI *)();
  if (auto tick = reinterpret_cast<Tick>(
          GetProcAddress(runtime.bootstrap, "timeGetTime")))
    tick();
  // The official, unmodified loader resolves the user's installed runtime.
  // Retain it for process lifetime because it owns process-local hooks.
  runtime.host = GetModuleHandleW(L"neuralhub_host.dll");
  if (!runtime.host) {
    log("installed NeuralHub runtime did not initialize");
    return false;
  }
  runtime.init = reinterpret_cast<Init>(
      GetProcAddress(runtime.host, "NVSDK_NGX_D3D12_Init_Ext"));
  runtime.populate = reinterpret_cast<Populate>(
      GetProcAddress(runtime.host, "NVSDK_NGX_D3D12_PopulateParameters_Impl"));
  runtime.create = reinterpret_cast<Create>(
      GetProcAddress(runtime.host, "NVSDK_NGX_D3D12_CreateFeature"));
  runtime.evaluate = reinterpret_cast<Evaluate>(
      GetProcAddress(runtime.host, "NVSDK_NGX_D3D12_EvaluateFeature"));
  runtime.release = reinterpret_cast<Release>(
      GetProcAddress(runtime.host, "NVSDK_NGX_D3D12_ReleaseFeature"));
  runtime.shutdown = reinterpret_cast<Shutdown>(
      GetProcAddress(runtime.host, "NVSDK_NGX_D3D12_Shutdown1"));
  runtime.caps = reinterpret_cast<Caps>(
      GetProcAddress(runtime.host, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
  runtime.destroyParams = reinterpret_cast<DestroyParams>(
      GetProcAddress(runtime.host, "NVSDK_NGX_D3D12_DestroyParameters"));
  getUI = reinterpret_cast<PFN_NVSDK_NGX_Parameter_GetUI>(
      GetProcAddress(runtime.host, "NVSDK_NGX_Parameter_GetUI"));
  getI = reinterpret_cast<PFN_NVSDK_NGX_Parameter_GetI>(
      GetProcAddress(runtime.host, "NVSDK_NGX_Parameter_GetI"));
  getResource = reinterpret_cast<PFN_NVSDK_NGX_Parameter_GetD3d12Resource>(
      GetProcAddress(runtime.host, "NVSDK_NGX_Parameter_GetD3d12Resource"));
  setUI = reinterpret_cast<PFN_NVSDK_NGX_Parameter_SetUI>(
      GetProcAddress(runtime.host, "NVSDK_NGX_Parameter_SetUI"));
  setI = reinterpret_cast<PFN_NVSDK_NGX_Parameter_SetI>(
      GetProcAddress(runtime.host, "NVSDK_NGX_Parameter_SetI"));
  setF = reinterpret_cast<PFN_NVSDK_NGX_Parameter_SetF>(
      GetProcAddress(runtime.host, "NVSDK_NGX_Parameter_SetF"));
  ready = runtime.init && runtime.caps && runtime.destroyParams &&
          runtime.create && runtime.evaluate && runtime.release &&
          runtime.shutdown && getUI && getI && getResource && setUI && setI &&
          setF;
  return ready;
}
constexpr auto fail = NVSDK_NGX_Result_FAIL_PlatformError;
} // namespace

extern "C" NVSDK_NGX_Result NVSDK_CONV VeyraRadeonFgInit(
    unsigned long long app, const wchar_t *path, ID3D12Device *device,
    NVSDK_NGX_Version version, const NVSDK_NGX_FeatureCommonInfo *info) {
  std::lock_guard lock(mutex);
  if (!device || !load())
    return fail;
  if (bootstrapDevice && bootstrapDevice != device) {
    log("DLSS-FG device changed; restart the player before changing GPU");
    return fail;
  }
  const auto result = runtime.init(app, path, device, version, info);
  log("DLSS-FG Radeon provider init result=" +
      std::to_string(unsigned(result)));
  if (result == NVSDK_NGX_Result_Success && !bootstrapSurface) {
    auto surface = std::make_unique<BootstrapSurface>();
    if (!surface->open(device)) {
      log("DLSS-FG hidden bootstrap failed");
      runtime.shutdown(device);
      return fail;
    }
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&VeyraRadeonFgInit),
                            &pinned)) {
      log("DLSS-FG bridge lifetime pin failed");
      runtime.shutdown(device);
      return fail;
    }
    bootstrapDevice = device;
    bootstrapSurface = surface.release();
    log("DLSS-FG display lifecycle initialized on Veyra's device");
  }
  return result;
}
extern "C" NVSDK_NGX_Result NVSDK_CONV
VeyraRadeonFgPopulate(NVSDK_NGX_Parameter *params) {
  std::lock_guard lock(mutex);
  if (!params || !runtime.caps)
    return fail;
  NVSDK_NGX_Parameter *capabilities = nullptr;
  const auto result = runtime.caps(&capabilities);
  int available = 0;
  unsigned maximum = 0;
  if (capabilities) {
    getI(capabilities, "FrameGeneration.Available", &available);
    getUI(capabilities, "DLSSG.MultiFrameCountMax", &maximum);
    runtime.destroyParams(capabilities);
  }
  log("DLSS-FG capability result=" + std::to_string(unsigned(result)) +
      " available=" + std::to_string(available) +
      " maximum=" + std::to_string(maximum));
  if (result != NVSDK_NGX_Result_Success || !available || !maximum)
    return fail;
  // First integration validates 2X only. Never advertise untested MFG here.
  setUI(params, "DLSSG.MultiFrameCountMax", 1u);
  setI(params, "FrameGeneration.Available", 1);
  log("DLSS-FG available; integration ceiling=2X");
  return result;
}
extern "C" NVSDK_NGX_Result NVSDK_CONV
VeyraRadeonFgCreate(ID3D12GraphicsCommandList *list, NVSDK_NGX_Feature feature,
                    NVSDK_NGX_Parameter *params, NVSDK_NGX_Handle **handle) {
  std::lock_guard lock(mutex);
  if (!runtime.create || !list || !params || !handle ||
      feature != NVSDK_NGX_Feature_FrameGeneration)
    return fail;
  unsigned w = 0, h = 0;
  getUI(params, "Width", &w);
  getUI(params, "Height", &h);
  const auto result = runtime.create(list, feature, params, handle);
  if (result == NVSDK_NGX_Result_Success && *handle)
    features[*handle] = {w, h, 0};
  log("DLSS-FG create " + std::to_string(w) + "x" + std::to_string(h) +
      " result=" + std::to_string(unsigned(result)));
  return result;
}
extern "C" NVSDK_NGX_Result NVSDK_CONV VeyraRadeonFgEvaluate(
    ID3D12GraphicsCommandList *list, const NVSDK_NGX_Handle *handle,
    NVSDK_NGX_Parameter *params, PFN_NVSDK_NGX_ProgressCallback callback) {
  std::lock_guard lock(mutex);
  auto it = features.find(handle);
  if (!list || !params || !runtime.evaluate || it == features.end())
    return fail;
  auto &feature = it->second;
  ID3D12Resource *depth = nullptr;
  ID3D12Resource *motion = nullptr;
  const auto depthResult =
      getResource(params, NVSDK_NGX_DLSSG_Parameter_Depth, &depth);
  const auto motionResult =
      getResource(params, NVSDK_NGX_DLSSG_Parameter_MVecs, &motion);
  unsigned count = 0;
  getUI(params, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, &count);
  if (!depth || !motion || !feature.width || !feature.height || count != 1) {
    log("input rejected depth=" + std::to_string(depth != nullptr) +
        " motion=" + std::to_string(motion != nullptr) +
        " count=" + std::to_string(count) +
        " depthResult=" + std::to_string(unsigned(depthResult)) +
        " motionResult=" + std::to_string(unsigned(motionResult)));
    return NVSDK_NGX_Result_FAIL_InvalidParameter;
  }
  // Veyra's original external slot is ANR: its bridge hands depth/motion over
  // as COPY_DEST and uses pixel motion. The DLSS path uses shader-readable
  // inputs and the same normalized sign convention as Veyra's NVIDIA path.
  D3D12_RESOURCE_BARRIER barriers[2]{};
  ID3D12Resource *resources[] = {depth, motion};
  for (unsigned i = 0; i < 2; ++i) {
    barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[i].Transition = {resources[i],
                              D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_COPY_DEST,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
  }
  list->ResourceBarrier(2, barriers);
  setF(params, NVSDK_NGX_DLSSG_Parameter_MvecScaleX, -1.f / feature.width);
  setF(params, NVSDK_NGX_DLSSG_Parameter_MvecScaleY, -1.f / feature.height);
  const auto result = runtime.evaluate(list, handle, params, callback);
  for (auto &barrier : barriers)
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
  list->ResourceBarrier(2, barriers);
  if (result == NVSDK_NGX_Result_Success)
    ++feature.evaluations;
  if (result != NVSDK_NGX_Result_Success || feature.evaluations == 1 ||
      feature.evaluations % 120 == 0)
    log("DLSS-FG evaluate completed=" + std::to_string(feature.evaluations) +
        " result=" + std::to_string(unsigned(result)));
  return result;
}
extern "C" NVSDK_NGX_Result NVSDK_CONV
VeyraRadeonFgRelease(NVSDK_NGX_Handle *handle) {
  std::lock_guard lock(mutex);
  if (!runtime.release)
    return fail;
  const auto result = runtime.release(handle);
  features.erase(handle);
  return result;
}
extern "C" NVSDK_NGX_Result NVSDK_CONV
VeyraRadeonFgShutdown(ID3D12Device *device) {
  std::lock_guard lock(mutex);
  if (!runtime.shutdown)
    return fail;
  const auto result = runtime.shutdown(device);
  log("DLSS-FG shutdown result=" + std::to_string(unsigned(result)));
  return result;
}
