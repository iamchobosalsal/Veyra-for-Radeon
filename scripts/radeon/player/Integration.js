// Appended only to the private Radeon integration package's EffectSupport.js.
function radeonChoices(api) {
    return api.fgBackendChoices.map(function(option) {
        if (option.id !== "anr") return option
        return { id: "anr", label: "DLSS-FG · Radeon", disabled: option.disabled || !api.amdNrGpu,
                 note: api.amdNrGpu ? (api.uiLanguage === "ko" ? "라데온에서 DLSS 프레임 생성 · 2X" : "DLSS Frame Generation on Radeon · 2X")
                     : (api.uiLanguage === "ko" ? "라데온 전용 · NVIDIA는 DLSS 프레임 생성 선택" : "Radeon only · select native DLSS Frame Generation on NVIDIA") }
    })
}
function radeonHint(api) {
    if (api.fgBackendName !== "anr") return api.fgProviderText
    var label = api.uiLanguage === "ko" ? "DLSS-FG · Radeon · 2X" : "DLSS-FG · Radeon · 2X"
    return label + (api.fgActive ? (api.uiLanguage === "ko" ? " · 처리 중" : " · processing")
                               : (api.uiLanguage === "ko" ? " · 재생 후 동작 상태 확인" : " · check status during playback"))
}
function radeonText(text) { return text.replace(/ANR FG/g, "DLSS-FG · Radeon") }
function radeonLabel(text) { return /^ANR FG/.test(text) ? "DLSS-FG · Radeon" : text }
function radeonMultipliers(api) {
    return api.fgBackendName === "anr" ? api.fgMultiplierChoices.filter(function(option) { return Number(option.id) <= 2 }) : api.fgMultiplierChoices
}
function radeonEffects(api) {
    return api.effectCatalog.map(function(option) {
        if (option.id !== "anr-fg") return option
        return Object.assign({}, option, {label: "DLSS-FG · Radeon", disabled: option.disabled || !api.amdNrGpu, note: "2X"})
    })
}
function applyRadeon(api) {
    var target = preferredNr(api)
    if (target < 0 || !gpuCapabilityAvailable(api, api.amdNrGpu ? "anr" : "dlss")) return
    if (!api.nrLayers.length) api.addEffect("nr")
    if (api.nrLayers.length) api.setNrLayerParameter(api.nrLayers[0].index, "runtime", target)
    api.videoSrQuality = api.amdNrGpu ? 5 : 0
    api.srTargetIndex = 2
    api.srEnabled = true
    api.nrEnabled = true
    api.fgBackendName = api.amdNrGpu ? "anr" : "dlss"
    api.fgMotionSource = api.amdNrGpu ? 1 : 0
    api.fgMultiplier = 2
    api.fgEnabled = true
}
function gpuCapabilityAvailable(api, key) { return !!(api.effectCapabilities[key] && api.effectCapabilities[key].available) }
function preferredNr(api) {
    if (api.amdNrGpu) return gpuCapabilityAvailable(api, "nr4") ? 4 : -1
    // Keep Veyra's normal fast SF default on RTX; the original RTX 50 model
    // remains selectable in the existing per-layer menu.
    if (gpuCapabilityAvailable(api, "nr2")) return 2
    return gpuCapabilityAvailable(api, "nr3") ? 3 : -1
}
function gpuPresetName(api) {
    return (api.amdNrGpu ? "Radeon" : (gpuCapabilityAvailable(api, "nr2") || gpuCapabilityAvailable(api, "nr3")) ? "NVIDIA" : "GPU") + " NR + DLSS-FG"
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
