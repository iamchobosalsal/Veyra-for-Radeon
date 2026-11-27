.pragma library

function status(api, id) {
    // Reading the notifying property also invalidates bindings after settings,
    // adapter/runtime status or a rejected backend request changes.
    var caps = api.effectCapabilities
    return typeof api.effectAvailability === "function"
            ? api.effectAvailability(id) : {available: true, reason: ""}
}
function available(api, id) { return status(api, id).available }
function reason(api, id) { return status(api, id).reason }
function option(api, id, label, key) {
    var cap = (api.effectCapabilities || {})[key] || {available: true, reason: ""}
    return {id: id, label: label, disabled: !cap.available, note: cap.reason}
}

function fgBackendShortLabel(option) {
    if (!option) return ""
    if (option.id === "vfg") return "NVIDIA VFG 👑"
    if (option.id === "anr") return "RIFE · Radeon"
    return option.label || ""
}

// Radeon integration preserved from the locally tested build (2026-11-26).
function radeonChoices(api) {
    return api.fgBackendChoices.map(function(option) {
        if (option.id !== "anr") return option
        return { id: "anr", label: "RIFE · Radeon", disabled: option.disabled || !api.amdNrGpu,
                 note: api.amdNrGpu ? (api.uiLanguage === "ko" ? "영상 전용 AI 프레임 생성 · 2X · 실험 기능" : "Video AI frame interpolation · 2X · experimental")
                     : (api.uiLanguage === "ko" ? "라데온 전용 · NVIDIA는 DLSS 프레임 생성 선택" : "Radeon only · select native DLSS Frame Generation on NVIDIA") }
    })
}
function radeonHint(api) {
    if (api.fgBackendName !== "anr") return api.fgProviderText
    var label = api.uiLanguage === "ko" ? "RIFE · Radeon · 2X" : "RIFE · Radeon · 2X"
    return label + (api.fgActive ? (api.uiLanguage === "ko" ? " · 처리 중" : " · processing")
                               : (api.uiLanguage === "ko" ? " · 재생 후 동작 상태 확인" : " · check status during playback"))
}
function radeonText(text) { return text.replace(/ANR FG/g, "RIFE · Radeon") }
function radeonLabel(text) { return /^ANR FG/.test(text) ? "RIFE · Radeon" : text }
function radeonMultipliers(api) {
    return api.fgBackendName === "anr" ? api.fgMultiplierChoices.filter(function(option) { return Number(option.id) <= 2 }) : api.fgMultiplierChoices
}
function radeonEffects(api) {
    return api.effectCatalog.map(function(option) {
        if (option.id !== "anr-fg") return option
        return Object.assign({}, option, {label: "RIFE · Radeon", disabled: option.disabled || !api.amdNrGpu, note: "2X"})
    })
}
function applyRadeon(api) {
    var target = preferredNr(api)
    if (target < 0 || !gpuCapabilityAvailable(api, api.amdNrGpu ? "anr" : "dlss")) return false
    if (!api.nrLayers.length && api.addEffect("nr") < 0) return false
    if (api.nrLayers.length) api.setNrLayerParameter(api.nrLayers[0].index, "runtime", target)
    api.videoSrQuality = api.amdNrGpu ? 5 : 0
    api.srTargetIndex = 2
    // Stage switches only edit existing nodes. Restore deleted preset stages
    // through the normal capability/chain validator, without duplicating them.
    if (!api.chain.some(function(node) { return node.type === "sr" })
            && api.addEffect("sr") < 0) return false
    api.srEnabled = true
    api.nrEnabled = true
    api.fgBackendName = api.amdNrGpu ? "anr" : "dlss"
    if (!api.chain.some(function(node) { return node.type === "frame-generation" })
            && api.addEffect(api.amdNrGpu ? "anr-fg" : "dlss-fg") < 0) return false
    api.fgMotionSource = api.amdNrGpu ? 1 : 0
    api.fgMultiplier = 2
    api.fgEnabled = true
    return true
}
function gpuCapabilityAvailable(api, key) { return !!(api.effectCapabilities[key] && api.effectCapabilities[key].available) }
function preferredNr(api) {
    if (api.amdNrGpu) return gpuCapabilityAvailable(api, "nr4") ? 4 : -1
    // Keep Veyra's normal fast SF default on RTX; the original RTX 50 model
    // remains selectable in the existing per-layer menu.
    if (gpuCapabilityAvailable(api, "nr2")) return 2
    return gpuCapabilityAvailable(api, "nr3") ? 3 : -1
}
// A missing local model is recoverable from the main button. Runtime/hardware
// failures still use the normal capability gate; connecting files applies no effects.
function gpuPresetState(api) {
    var assets = api.nrModelAssets || {}
    var caps = api.effectCapabilities || {}
    var ko = api.uiLanguage === "ko"
    var label = gpuPresetName(api)
    if (api.amdNrGpu && assets.managed && assets.busy)
        return {kind: "busy", enabled: false, label: ko ? "NR 모델 확인 중…" : "Checking NR models…",
                reason: ko ? "모델 파일 확인이 끝날 때까지 기다려 주세요." : "Wait for the model check to finish."}
    if (api.amdNrGpu && assets.managed && caps.nr4 && caps.nr4.modelAssetsMissing)
        return {kind: "connect", enabled: true, label: ko ? "NR 모델 연결" : "Connect NR models",
                reason: caps.nr4.reason || (ko ? "기존 NR 모델 폴더를 선택하세요." : "Select your existing NR model folder.")}
    var target = preferredNr(api)
    if (target < 0) {
        var nr = caps[api.amdNrGpu ? "nr4" : "nr2"] || {}
        return {kind: "blocked", enabled: false, label: label,
                reason: nr.reason || (ko ? "현재 GPU에서 사용할 수 있는 NR 실행 구성이 없습니다." : "No compatible NR runtime is available on this GPU.")}
    }
    var fg = caps[api.amdNrGpu ? "anr" : "dlss"] || {}
    if (!fg.available)
        return {kind: "blocked", enabled: false, label: label,
                reason: fg.reason || (ko ? "프레임 생성 실행 구성을 사용할 수 없습니다." : "The frame generation runtime is unavailable.")}
    return {kind: "apply", enabled: true, label: label,
            reason: ko ? "NR 화질 개선 + 4K 출력 + 선택한 GPU의 프레임 생성 2X" : "NR enhancement + 4K output + frame generation for the selected GPU"}
}
function activateGpuPreset(api) {
    var state = gpuPresetState(api)
    if (!state.enabled) return false
    if (state.kind === "connect") api.chooseNrModelFolder()
    else if (state.kind === "apply") return applyRadeon(api)
    else return false
    return true
}
function nrModelConnectionText(api) {
    var assets = api.nrModelAssets || {}
    var cap = (api.effectCapabilities || {}).nr4 || {}
    var ko = api.uiLanguage === "ko"
    if (assets.busy) return ko ? "모델 파일 확인 중…" : "Checking model files…"
    var message = assets.error || ""
    if (!message && !cap.available)
        message = cap.reason || (ko ? "NR 모델이 연결되지 않았습니다. 기존 모델 폴더 또는 이전 Veyra 폴더를 선택하세요." : "NR models are not connected. Select an existing model folder or an old Veyra folder.")
    if (!message) {
        var models = []
        if (assets.smallReady) models.push("Small")
        if (assets.originalReady) models.push("Original")
        message = (ko ? "모델 파일 확인됨: " : "Model files checked: ") + models.join(" / ")
    }
    if (assets.restartRequired)
        message += ko ? "\n실행 중인 NR에 새 모델 폴더를 적용하려면 앱을 재시작하세요." : "\nRestart to use the new model folder in the active NR pipeline."
    return message + (assets.directory ? "\n" + assets.directory : "")
}
function gpuPresetName(api) {
    return (api.amdNrGpu ? "Radeon" : (gpuCapabilityAvailable(api, "nr2") || gpuCapabilityAvailable(api, "nr3")) ? "NVIDIA" : "GPU") + (api.amdNrGpu ? " NR + RIFE" : " NR + DLSS-FG")
}
function reconcileGpu(api) {
    var wanted = api.preferences.monitorGpu || ""
    if (wanted && !api.gpuMonitorChoices.some(function(g) { return g.id === wanted })) api.setPreference("monitorGpu", "")
    var target = preferredNr(api)
    var layers = api.nrLayers
    if (target >= 0 && layers.some(function(layer) { return !gpuCapabilityAvailable(api, "nr" + layer.runtime) })) {
        api.setNrLayerParameter(layers[0].index, "runtime", target)
    }
    var nativeFg = api.amdNrGpu ? "anr" : "dlss"
    if (gpuCapabilityAvailable(api, nativeFg) && ((!api.amdNrGpu && api.fgBackendName === "anr") || !gpuCapabilityAvailable(api, api.fgBackendName))) {
        var enabled = api.fgEnabled
        api.fgBackendName = nativeFg
        api.fgEnabled = enabled
    }
}
