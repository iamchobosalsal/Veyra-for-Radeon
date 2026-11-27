// QML bridge: Hardware responsibilities.
#include "../QmlPlayerBridge_p.h"
#include "veyra/pipeline/Rdna3NrBackend.h"
#include "veyra/pipeline/AmdNrProvider.h"
#include "veyra/pipeline/VfxSrBackend.h"
#include "veyra/ngx/AnrProvider.h"
#include "veyra/gfx/AmfFrcBackend.h"
#include "veyra/gfx/FsrSrBackend.h"
#include "veyra/gfx/FsrFgPresenter.h"
#include "veyra/gfx/FsrRuntimeSelection.h"
#include "veyra/guidance/AmdOpticalFlow.h"
#include "veyra/base/NrModelAssets.h"
#include <QThread>

namespace veyra::ui {
namespace {
// Only the UI thread reads/writes this cache. Workers return isolated reports;
// snapshot bindings never traverse hundreds of model files.
struct NrAssetsCache {
    base::NrModelAssets assets;
    bool checked=false,busy=false,restartRequired=false;
    QString error;
};
NrAssetsCache& nrAssetsCache(){static NrAssetsCache cache;return cache;}
QString nrText(const char* korean,const char* english){return QString::fromUtf8(i18n::current()==QStringLiteral("ko")?korean:english);}
QString nrAssetReason(bool performance){
    const auto& cache=nrAssetsCache();
    if(!cache.checked)return nrText("NR 모델 폴더를 확인하고 있습니다.","Checking the local NR model folder.");
    const auto& model=performance?cache.assets.smallModel:cache.assets.originalModel;
    if(model.ready)return {};
    return QString(performance?"Small: ":"Original: ")+QString::fromStdString(model.issue)+" · "+
        nrText("로컬 NR 모델 폴더를 선택해 주세요.","Select your local NR model folder.");
}
}


// The first hardware adapter, the one the engine opens (gfx adapter[0]): 0x10DE NVIDIA,
// 0x1002 AMD, 0x8086 Intel, 0 unknown.
uint32_t primaryGpuVendor() {
    return effectGpu().vendor;
}
// FSR 4 ML frame generation needs an AMD RX 9000; on other GPUs the provider crashed the
// whole program (field report 2026-10-01). Known non-AMD adapters are refused up front.
bool fsr4Possible() { return currentEffectCapabilities().value("fsr4").toMap().value("available").toBool(); }
// The adapter the engine opens: the user's compute GPU when one is chosen
// (gfx::preferredAdapterLuid) and usable, else the first D3D12 hardware
// adapter in DXGI's high-performance order, as D3D12DeviceContext picks it.
// UI thread only; cached per preference.
const DXGI_ADAPTER_DESC1* engineAdapterDesc(){
    static uint64_t cachedFor=~0ull;static DXGI_ADAPTER_DESC1 cached{};static bool found=false;
    const uint64_t preferred=gfx::preferredAdapterLuid();
    if(cachedFor==preferred)return found?&cached:nullptr;
    cachedFor=preferred;found=false;
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return nullptr;
    for(int pass=preferred?0:1;pass<2&&!found;++pass)
        for(UINT i=0;;++i){Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if(FAILED(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter)))||!adapter)break;
            DXGI_ADAPTER_DESC1 d{};
            if(FAILED(adapter->GetDesc1(&d))||(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))continue;
            const uint64_t luid=uint64_t(uint32_t(d.AdapterLuid.HighPart))<<32|d.AdapterLuid.LowPart;
            if(pass==0&&luid!=preferred)continue;
            if(FAILED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,__uuidof(ID3D12Device),nullptr)))continue;
            cached=d;found=true;break;
        }
    return found?&cached:nullptr;
}
// The menu checks the same adapter as the engine. The native backend then
// verifies CUDA compute capability and the exact adapter LUID.
bool vfgHardwarePossible(){
    const auto* d=engineAdapterDesc();
    if(!d)return false;
    const auto name=QString::fromWCharArray(d->Description);
    return d->VendorId==0x10DE&&(name.contains(QStringLiteral("GeForce RTX 30"),Qt::CaseInsensitive)||name.contains(QStringLiteral("GeForce RTX 40"),Qt::CaseInsensitive)||
        name.contains(QStringLiteral("GeForce RTX 50"),Qt::CaseInsensitive)||name.contains(QStringLiteral("Ada"),Qt::CaseInsensitive)||name.contains(QStringLiteral("Blackwell"),Qt::CaseInsensitive));
}
unsigned normalizeFgMultiplier(unsigned value,engine::FrameGenerationBackend backend,unsigned cap){
    value=std::clamp(value,2u,cap);
    return backend==engine::FrameGenerationBackend::Dlss&&value==5?4u:value;
}
bool QmlPlayerBridge::amdNrGpu() const { return primaryGpuVendor()==0x1002; }

ui::EffectGpu effectGpu() {
    ui::EffectGpu result;
    const auto* d = engineAdapterDesc();
    if (!d) return result;
    const auto name = QString::fromWCharArray(d->Description);
    result.vendor = d->VendorId;
    result.deviceId = d->DeviceId;
    result.rtx = name.contains("RTX", Qt::CaseInsensitive);
    result.blackwell = name.contains("RTX 50", Qt::CaseInsensitive) || name.contains("Blackwell", Qt::CaseInsensitive);
    result.ada = name.contains("RTX 40", Qt::CaseInsensitive) || name.contains("Ada", Qt::CaseInsensitive);
    result.ampere = name.contains("GeForce RTX 30", Qt::CaseInsensitive);
    result.rx9000 = name.contains("RX 90", Qt::CaseInsensitive);
    result.rx7000 = pipeline::amdNrProvider(d->VendorId,d->Description)==pipeline::AmdNrProvider::RedDukeRdna3;
    return result;
}

static QVariantMap scanEffectCapabilities() {
    using H = ui::HardwareEffect;
    const auto gpu = effectGpu();
    const auto nr = runtime::localRuntimeDirectory();
    const auto data = runtime::localDataDirectory();
    auto exists = [](const std::filesystem::path& path) { std::error_code ec; return std::filesystem::is_regular_file(path, ec); };
    const auto fsr = data / "amd/fidelityfx";
    const auto fsrRuntime = gfx::findFsrRuntime(fsr);
    // NVIDIA can supply NVOF; Radeon/Intel use the FidelityFX flow backend.
    const bool fgFlowCompiled = gpu.vendor == 0x10de || guidance::AmdOpticalFlow::compiledIn();
    QVariantMap out;
    auto add = [&](const QString& id, H hardware, bool files, const QString& requirement, bool compiled = true) {
        const bool supported = ui::hardwareSupports(hardware, gpu);
        out[id] = QVariantMap{{"available", supported && compiled && files}, {"reason", !supported ? requirement : !compiled ? QObject::tr("此构建未包含该后端") : !files ? QObject::tr("当前包缺少此功能的运行组件") : QString{}}};
    };
    add("nr2", H::NvidiaNrSf, exists(nr / "nr-ampere/nvngx_dlssnr.dll"), QObject::tr("需要 NVIDIA RTX 显卡"));
    out["nr0"]=out["nr2"]; // Retired preset ID; no retired runtime is loaded.
    out["nrAuto"]=QVariantMap{{"available",gpu.vendor==0x10DE&&gpu.deviceId==0x2f04&&out.value("nr0").toMap().value("available").toBool()},
        {"reason",gpu.deviceId!=0x2f04?QObject::tr("自动 NR 当前仅验证 RTX 5070"):!out.value("nr0").toMap().value("available").toBool()?QObject::tr("当前包缺少此功能的运行组件"):QString{}}};
    add("nr3", H::NvidiaNr50, exists(nr / "nr-original/nvngx_dlssnr.dll"), QObject::tr("需要 NVIDIA RTX 50 / Blackwell"));
    const auto rdna3Directory=(nr/"../amd-nr-rdna3").lexically_normal();
    const bool amdNrFiles=gpu.rx7000?pipeline::Rdna3NrBackend::runtimeAvailable(rdna3Directory):
        (exists(nr / "../amd-nr/LmxxfNrRuntime.dll")||exists(nr / "../amd-nr/LmxxfNrRuntime-Original.dll"));
    add("nr4", H::AmdNr, amdNrFiles, QObject::tr("需要 AMD RX 7000 / 9000"));
    if(gpu.rx7000&&!amdNrFiles){auto cap=out.value("nr4").toMap();cap["reason"]=QObject::tr("当前包缺少 RX 7000 NR 运行组件");out["nr4"]=cap;}
    wchar_t systemDir[MAX_PATH]{};
    const bool hipMissing=gpu.rx7000?!pipeline::Rdna3NrBackend::hipRuntimeAvailable():
        GetSystemDirectoryW(systemDir, MAX_PATH)&&!exists(std::filesystem::path(systemDir)/"amdhip64_7.dll")&&!GetModuleHandleW(L"amdhip64_7.dll");
    if (hipMissing) {
        auto cap = out.value("nr4").toMap();
        if (cap.value("available").toBool()) { cap["available"] = false; cap["reason"] = QObject::tr("需要 AMD HIP 7 驱动运行组件"); out["nr4"] = cap; }
    }
    if(gpu.rx9000){
        auto cap=out.value("nr4").toMap();
        cap["runtimeAvailable"]=cap.value("available");
        const auto& assets=nrAssetsCache();
        cap["smallRuntimeAvailable"]=exists(nr/"../amd-nr/LmxxfNrRuntime.dll");
        cap["originalRuntimeAvailable"]=exists(nr/"../amd-nr/LmxxfNrRuntime-Original.dll");
        const bool smallReady=assets.checked&&assets.assets.smallModel.ready&&cap.value("smallRuntimeAvailable").toBool();
        const bool originalReady=assets.checked&&assets.assets.originalModel.ready&&cap.value("originalRuntimeAvailable").toBool();
        if(cap.value("available").toBool()&&!smallReady&&!originalReady){
            cap["available"]=false;cap["modelAssetsMissing"]=true;
            cap["reason"]=nrAssetReason(true);out["nr4"]=cap;
        }else out["nr4"]=cap;
    }
    add("sr0", H::Nvidia, exists(nr / "nvngx_dlss.dll"), QObject::tr("需要 NVIDIA RTX 显卡"));
    for (int i = 1; i <= 4; ++i) add("sr" + QString::number(i), H::Nvidia, exists(nr / "nvngx_vsr.dll"), QObject::tr("需要 NVIDIA RTX 显卡"));
    // RTX Video SR high bitrate / streaming: Video Effects VideoSuperRes, from the VFG runtime directory.
    const bool vfxSr = pipeline::VfxSrBackend::runtimeAvailable(nr.wstring());
    for (int q : {16, 17, 18, 19}) add("sr" + QString::number(q), H::Nvidia, vfxSr, QObject::tr("需要 NVIDIA RTX 显卡"));
    for (int q : {21, 23}) add("sr" + QString::number(q), H::NvidiaAmpere, vfxSr, QObject::tr("需要 NVIDIA RTX 30 及更新显卡"));
    add("sr5", H::CrossVendor, fsrRuntime.upscale, QObject::tr("需要支持 DirectX 12 的硬件显卡"), gfx::FsrSrBackend::compiledIn());
    add("anr", H::CrossVendor, ngx::AnrProvider::available(nr.wstring()), QObject::tr("需要支持 DirectX 12 的硬件显卡"));
    add("dlss", H::Nvidia, exists(nr / "nvngx_dlssg.dll"), QObject::tr("需要 NVIDIA RTX 显卡"));
    add("vfg", H::Vfg, pipeline::VfgBackend::runtimeAvailable(nr.wstring()), QObject::tr("需要 NVIDIA RTX 30 / 40 / 50（30 系为实验兼容）"));
    add("xess", H::CrossVendor, exists(data / "intel/experimental/libxess_fg.dll") && exists(data / "intel/experimental/libxell.dll"), QObject::tr("需要支持 DirectX 12 的硬件显卡"));
    add("fsr3", H::CrossVendor, fsrRuntime.frameGeneration, QObject::tr("需要支持 DirectX 12 的硬件显卡"), gfx::FsrFgPresenter::compiledIn() && fgFlowCompiled);
    add("fsr4", H::Fsr4, fsrRuntime.mlFrameGeneration, QObject::tr("需要 AMD RX 9000"), gfx::FsrFgPresenter::mlCompiledIn() && fgFlowCompiled);
    // AMD FRC uses the AMF runtime the AMD driver installs; nothing ships with Veyra.
    add("amdfrc", H::AmdFrc, gfx::AmfFrcBackend::runtimeInstalled(), QObject::tr("需要 AMD 显卡（驱动自带 AMF 运行库）"), gfx::AmfFrcBackend::compiledIn());
    add("hdr", H::CrossVendor,
        gpu.vendor == 0x10de ? exists(nr / "nvngx_truehdr.dll")
                             : exists(runtime::shaderDirectory() / "VideoHdrExpand.dxil"),
        QObject::tr("需要支持 DirectX 12 的硬件显卡"));
    auto hdr = out.value("hdr").toMap();
    hdr["label"] = gpu.vendor == 0x10de ? QStringLiteral("RTX Video HDR")
        : gpu.vendor == 0x1002 ? QStringLiteral("Video HDR · Radeon")
                              : QStringLiteral("Video HDR · DirectX");
    out["hdr"] = hdr;
    add("flow0", H::Nvidia, true, QObject::tr("需要 NVIDIA RTX 显卡及 NVOF 驱动"));
    add("flow1", H::CrossVendor, true, QObject::tr("需要支持 DirectX 12 的硬件显卡"), guidance::AmdOpticalFlow::compiledIn());
    return out;
}
// All callers are on the bridge/UI thread. Snapshot reads must not perform
// filesystem probes or rebuild capability maps. Refresh on actions, language
// changes and a slow timer so user-replaced runtime files remain discoverable.
QVariantMap currentEffectCapabilities(bool refresh) {
    static QVariantMap cached;
    static QString language;
    const auto currentLanguage=i18n::current();
    if(refresh||cached.isEmpty()||language!=currentLanguage){
        cached=scanEffectCapabilities();language=currentLanguage;
    }
    return cached;
}
QString fgEffectId(engine::FrameGenerationBackend backend) {
    switch (backend) {
    case engine::FrameGenerationBackend::Dlss: return "dlss";
    case engine::FrameGenerationBackend::XeSS: return "xess";
    case engine::FrameGenerationBackend::Fsr: return "fsr3";
    case engine::FrameGenerationBackend::Fsr4: return "fsr4";
    case engine::FrameGenerationBackend::Vfg: return "vfg";
    case engine::FrameGenerationBackend::Anr: return "anr";
    case engine::FrameGenerationBackend::AmdFrc: return "amdfrc";
    }
    return {};
}
QString unavailableEffects(const engine::EnhancementSettings& settings) {
    const auto caps = currentEffectCapabilities(true);
    if(settings.nr&&engine::currentNrRuntime(settings.nrRuntime)==engine::NrRuntime::LmxxfAmd&&effectGpu().rx9000){
        for(uint32_t i=0;i<std::max(1u,settings.nrLayerCount);++i){
            const auto layer=settings.nrLayer(i);if(!layer.enabled)continue;
            const auto reason=nrAssetReason(layer.model.performance);if(!reason.isEmpty())return reason;
            if(!caps.value("nr4").toMap().value(layer.model.performance?"smallRuntimeAvailable":"originalRuntimeAvailable").toBool())
                return nrText("선택한 NR 모델의 실행 파일이 없습니다.","The selected NR model runtime is missing.");
        }
    }
    QStringList requested;
    if (settings.nr) requested << "nr" + QString::number(int(engine::currentNrRuntime(settings.nrRuntime)));
    if (settings.sr) requested << "sr" + QString::number(settings.videoSrQuality);
    // Double SR runs a second pass with its own backend (any SR algorithm).
    if (settings.sr && settings.dualSrEnabled) requested << "sr" + QString::number(settings.dualSrQuality);
    if (settings.multiplier > 1) requested << fgEffectId(settings.frameGenerationBackend);
    if (settings.videoHdr.enabled) requested << "hdr";
    for (const auto& id : requested) {
        const auto cap = caps.value(id).toMap();
        if (!cap.value("available").toBool()) return id + ": " + cap.value("reason").toString();
    }
    return {};
}
QString disableUnavailableEffects(engine::ChainConfiguration& configuration) {
    const auto caps = currentEffectCapabilities();
    const auto available = [&](const char* id) { return caps.value(id).toMap().value("available").toBool(); };
    const engine::FrameGenerationAvailability fgAvailable{
        available("dlss"),available("xess"),available("fsr3"),available("fsr4"),available("vfg"),available("anr"),available("amdfrc")};
    QStringList reasons;
    const auto sanitize = [&](engine::EffectChain& chain, engine::ChainGlobalSettings& globals) {
        // Also migrate disabled stages: keeping an unavailable DLSS selection
        // left the AMD dropdown at DLSS even though a fresh profile chose FSR.
        const auto savedBackend = globals.fgBackend;
        globals.fgBackend = engine::restoredFrameGenerationBackend(savedBackend,fgAvailable);
        if (globals.fgBackend != savedBackend) {
            if (engine::twoXFrameGeneration(globals.fgBackend)) chain.fgMultiplier=2;
            else if (globals.fgBackend==engine::FrameGenerationBackend::XeSS)
                chain.fgMultiplier=normalizeFgMultiplier(chain.fgMultiplier,globals.fgBackend,2);
            else chain.fgMultiplier=normalizeFgMultiplier(chain.fgMultiplier,globals.fgBackend,6);
            veyra::log::warn("fg-default",std::format("unavailable saved backend={} selected={} rememberedMultiplier={} enabled flags preserved",
                engine::frameGenerationBackendName(savedBackend),engine::frameGenerationBackendName(globals.fgBackend),chain.fgMultiplier));
        }
        for (uint32_t i = 0; i < chain.nodeCount; ++i) {
            auto& node = chain.nodes[i];
            QString id;
            switch (node.type) {
            case engine::EffectType::NrEnhance: id = "nr" + QString::number(int(engine::currentNrRuntime(node.nr.runtime))); break;
            case engine::EffectType::SuperResolution: id = "sr" + QString::number(globals.videoSrQuality); break;
            case engine::EffectType::FrameGeneration: id = fgEffectId(globals.fgBackend); break;
            case engine::EffectType::VideoHdr: id = "hdr"; break;
            default: break;
            }
            if (id.isEmpty() || !node.enabled) continue;
            const auto cap = caps.value(id).toMap();
            if (!cap.value("available").toBool()) {
                if(node.type==engine::EffectType::NrEnhance&&cap.value("modelAssetsMissing").toBool())continue;
                node.enabled = false;
                reasons << id + ": " + cap.value("reason").toString();
            }
        }
        if (globals.opticalFlowBackend == engine::OpticalFlowBackend::Nvidia &&
            !caps.value("flow0").toMap().value("available").toBool())
            globals.opticalFlowBackend = engine::OpticalFlowBackend::AmdFidelityFx;
        // A double SR second algorithm this card cannot run (a profile moved from
        // an NVIDIA card) follows the base algorithm, or double SR turns off.
        const auto runs = [&](uint32_t quality) { return caps.value("sr" + QString::number(quality)).toMap().value("available").toBool(); };
        if (globals.dualSrEnabled && !runs(globals.dualSrQuality)) {
            const auto saved = globals.dualSrQuality;
            if (runs(globals.videoSrQuality)) globals.dualSrQuality = globals.videoSrQuality;
            else globals.dualSrEnabled = false;
            veyra::log::warn("effect-availability", std::format("double SR second quality={} unavailable; second={} enabled={}",
                saved, globals.dualSrQuality, globals.dualSrEnabled));
        }
    };
    sanitize(configuration.chain, configuration);
    if (configuration.editor) {
        auto editor = std::make_shared<engine::NodeEditorDocument>(*configuration.editor);
        auto globals = editor->globals.value_or(static_cast<const engine::ChainGlobalSettings&>(configuration));
        sanitize(editor->nodes, globals);
        editor->globals = globals;
        configuration.editor = std::move(editor);
    }
    reasons.removeDuplicates();
    return reasons.join("; ");
}
QVariantMap QmlPlayerBridge::effectCapabilities() const { return currentEffectCapabilities(); }
QVariantMap QmlPlayerBridge::effectAvailability(const QString& id) const {
    const auto& s = settings();
    const QString key = id == "nr" ? "nr" + QString::number(int(engine::currentNrRuntime(s.nrRuntime))) :
        id == "sr" ? "sr" + QString::number(s.videoSrQuality) : id == "video-hdr" ? QString("hdr") :
        id == "frame-generation" ? fgEffectId(s.frameGenerationBackend) :
        id == "dlss-fg" ? QString("dlss") : id == "xess-fg" ? QString("xess") :
        id == "fsr3-fg" ? QString("fsr3") : id == "fsr4-fg" ? QString("fsr4") : id == "vfg-fg" ? QString("vfg") : id == "anr-fg" ? QString("anr") : id == "amdfrc-fg" ? QString("amdfrc") : id;
    const auto caps = effectCapabilities();
    return caps.contains(key) ? caps.value(key).toMap() : QVariantMap{{"available", true}, {"reason", QString{}}};
}
QVariantList QmlPlayerBridge::nrRuntimeChoices() const {
    QVariantList choices;
    const auto caps = effectCapabilities();
    for (const auto& entry : std::initializer_list<std::pair<int, QString>>{{2, "RTX 20–50 · SF 310.9.16"}, {3, tr("RTX 50 · NVIDIA 原版")}, {4, effectGpu().rx7000?tr("RX 7000 · RedDuke（实验）"):tr("RX 9000 · lmxxf（实验）")}}) {
        const auto cap = caps.value("nr" + QString::number(entry.first)).toMap();
        choices << QVariantMap{{"id", QString::number(entry.first)}, {"label", entry.second}, {"disabled", !cap.value("available").toBool()}, {"note", cap.value("reason")}};
    }
    return choices;
}
QVariantList QmlPlayerBridge::srBackendChoices() const {
    QVariantList choices;
    const auto caps = effectCapabilities();
    // RTX Video SR families take string ids (srAlgorithmOf); the tier is chosen
    // separately. Availability follows each family's runtime.
    struct Entry { QString id; int capability; QString label; };
    for (const auto& entry : std::initializer_list<Entry>{{"0", 0, "DLSS SR"}, {"rtx", 4, tr("RTX 视频超分 · 标准")},
            {"rtx-hb", 16, tr("RTX 视频超分 · 高码率")}, {"rtx-st", 21, tr("RTX 视频超分 · 流媒体")},
            {"5", 5, tr("FSR · 视频估算输入（实验）")}}) {
        const auto cap = caps.value("sr" + QString::number(entry.capability)).toMap();
        choices << QVariantMap{{"id", entry.id}, {"label", entry.label}, {"disabled", !cap.value("available").toBool()}, {"note", cap.value("reason")}};
    }
    return choices;
}



QVariantMap QmlPlayerBridge::nrModelAssets() const {
    const auto& c=nrAssetsCache();
    return {{"managed",effectGpu().rx9000},{"directory",QString::fromStdWString(c.assets.directory.wstring())},
        {"busy",c.busy},{"checked",c.checked},{"smallReady",c.checked&&c.assets.smallModel.ready},
        {"originalReady",c.checked&&c.assets.originalModel.ready},{"smallReason",nrAssetReason(true)},
        {"originalReason",nrAssetReason(false)},{"error",c.error},{"restartRequired",c.restartRequired}};
}
QVariantMap QmlPlayerBridge::nrModelAvailability(bool performance) const {
    auto result=effectCapabilities().value("nr4").toMap();
    if(!effectGpu().rx9000)return result;
    const auto& c=nrAssetsCache();const auto& model=performance?c.assets.smallModel:c.assets.originalModel;
    const bool runtimePresent=result.value(performance?"smallRuntimeAvailable":"originalRuntimeAvailable").toBool();
    const bool hardware=result.value("runtimeAvailable").toBool();
    result["available"]=hardware&&runtimePresent&&c.checked&&model.ready;
    if(hardware)result["reason"]=!runtimePresent?nrText("선택한 NR 모델의 실행 파일이 없습니다.","The selected NR model runtime is missing."):nrAssetReason(performance);
    result["model"]=performance?"Small":"Original";return result;
}
void QmlPlayerBridge::chooseNrModelFolder(){
    if(nrAssetsCache().busy)return;
    const auto folder=QFileDialog::getExistingDirectory(nullptr,nrText("로컬 NR 모델 폴더 또는 이전 Veyra 폴더 선택","Select local NR models or an old Veyra folder"),
        nrModelAssets().value("directory").toString());
    if(!folder.isEmpty())importNrModelFolder(folder);
}
void QmlPlayerBridge::importNrModelFolder(const QString& folder){checkNrModelFolder(folder,true);}
void QmlPlayerBridge::refreshNrModelAssets(){checkNrModelFolder({},false);}
void QmlPlayerBridge::checkNrModelFolder(const QString& folder,bool remember){
    auto& cache=nrAssetsCache();if(cache.busy)return;
    cache.busy=true;cache.error.clear();emit nrModelAssetsChanged();
    const auto reference=runtime::userDataDirectory()/"radeon/nr-assets-path.txt";
    const auto fallback=runtime::radeonNrAssetsDirectory((runtime::localRuntimeDirectory()/"../amd-nr/assets").lexically_normal());
    struct Outcome{base::NrModelAssets assets;QString error;bool saved=false;};
    auto outcome=std::make_shared<Outcome>();
    // Metadata and file I/O stay off the GUI thread. No model copies, downloads,
    // weight hashes, GPU initialization, or changes to the selected source tree.
    auto* worker=QThread::create([outcome,folder,remember,reference,fallback]{
        try{
            if(remember)outcome->assets=base::findNrModelAssets(std::filesystem::path(folder.toStdWString()));
            else{
                std::string issue;const auto selected=base::readNrModelAssetsReference(reference,&issue);
                outcome->assets=base::inspectNrModelAssets(selected.empty()?fallback:selected);
                if(!issue.empty()){
                    outcome->assets.shared=outcome->assets.smallModel=outcome->assets.originalModel={false,issue};
                }
            }
            if(remember){
                if(!outcome->assets.anyReady()){
                    outcome->error=QString::fromStdString(outcome->assets.shared.ready?
                        outcome->assets.smallModel.issue+"; "+outcome->assets.originalModel.issue:outcome->assets.shared.issue);return;
                }
                std::error_code ec;std::filesystem::create_directories(reference.parent_path(),ec);
                if(ec){outcome->error=QString::fromStdString(ec.message());return;}
                QSaveFile file(QString::fromStdWString(reference.wstring()));
                const auto path=QString::fromStdWString(outcome->assets.directory.wstring()).toUtf8()+"\n";
                if(!file.open(QIODevice::WriteOnly)||file.write(path)!=path.size()||!file.commit()){
                    outcome->error=file.errorString();return;
                }
                outcome->saved=true;
            }
        }catch(const std::exception& e){
            outcome->error=QString::fromUtf8(e.what());
            outcome->assets.shared=outcome->assets.smallModel=outcome->assets.originalModel={false,e.what()};
        }
    });
    connect(worker,&QThread::finished,worker,&QObject::deleteLater);
    connect(worker,&QThread::finished,this,[this,outcome,remember]{
        auto& c=nrAssetsCache();c.busy=false;
        if(remember&&!outcome->saved){
            c.error=nrText("폴더를 등록하지 못했습니다: ","Could not select model folder: ")+outcome->error;
            emit nrModelAssetsChanged();emit notice(c.error,true);return;
        }
        c.assets=std::move(outcome->assets);c.checked=true;c.error=outcome->error;
        if(remember)c.restartRequired=false;
        currentEffectCapabilities(true);emit nrModelAssetsChanged();emit effectCapabilitiesChanged();emit snapshotChanged();
        if(!remember)return;
        // Restore from the editor chain, not the engine's NR-disabled recovery
        // settings. A failed ON layer can therefore retry without a manual toggle.
        bool retried=false;
        if(impl_->revalidate()){
            auto desired=impl_->facade.pendingSettings();
            if(desired.nr&&engine::currentNrRuntime(desired.nrRuntime)==engine::NrRuntime::LmxxfAmd){
                const auto reason=unavailableEffects(desired);
                if(!reason.isEmpty())c.error=reason;
                else if(queuedFramesKnown()&&impl_->snapshot.applied.nr)c.restartRequired=true;
                else {
                    retried=impl_->facade.applySettings(desired,engine::runtimeOrder(impl_->activeRuntime()));
                    if(!retried)c.error=nrText("모델 폴더는 저장했지만 NR 재시도를 제출하지 못했습니다.","Model folder saved, but the NR retry could not be submitted.");
                }
            }
        }else c.error=uiText(impl_->validation.message);
        emit nrModelAssetsChanged();emit settingsChanged();emitChainChanged();
        if(!c.error.isEmpty())emit notice(c.error,true);
        else emit notice(c.restartRequired?
            nrText("모델 폴더를 저장했습니다. 실행 중인 NR에 적용하려면 앱을 재시작해 주세요.","Model folder saved. Restart the app to replace models in the active NR pipeline."):
            retried?nrText("모델 폴더를 저장했습니다. 켜 둔 NR 레이어를 다시 적용합니다.","Model folder saved. Enabled NR layers have been submitted again."):
            nrText("모델 폴더를 저장했습니다.","Model folder saved."),false);
    });
    worker->start();
}

} // namespace veyra::ui
