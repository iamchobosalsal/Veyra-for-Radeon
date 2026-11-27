import QtQuick
Item {
    id: test
    property string media: ""
    property int ticks: 0
    property int phase: 0
    property int activeTicks: 0
    property bool combined: false
    property bool lifecycle: false
    property bool fast60: false
    property bool gpuSwitch: false
    property bool menuControls: false
    property bool vfgHigh: false
    property int phaseTicks: 0
    property int cycle: 0
    function named(item,name) {
        if(item.objectName===name) return item
        var children=item.children || []
        for(var i=0;i<children.length;++i) { var found=named(children[i],name); if(found) return found }
        return null
    }
    function check(ok, reason) { if (!ok) throw new Error(reason) }
    Timer { id: clock; interval: 250; repeat: true; running: test.media !== ""
        onTriggered: { try {
            if (++test.ticks > 560) throw new Error("140s deadline: " + veyra.runStatusDetail)
            if(test.ticks===2) root.page="pro"
            if (test.phase === 0 && test.ticks >= 8) {
                if(test.gpuSwitch) {
                    console.log("GPU_SWITCH_STATE",JSON.stringify({gpu:veyra.gpuMonitorName,caps:veyra.effectCapabilities,layers:veyra.nrLayers,fg:veyra.fgBackendName,monitor:veyra.preferences.monitorGpu}))
                    check(!veyra.amdNrGpu,"NVIDIA adapter was not detected")
                    check(veyra.gpuMonitorName.indexOf("RTX 5080")>=0,"Expected the installed RTX 5080")
                    check(!veyra.preferences.monitorGpu,"Stale Radeon preference survived migration")
                    check(veyra.nrLayers.every(function(layer) { return layer.runtime!==4 }),"Radeon NR selection survived migration")
                    check(veyra.fgBackendName==="dlss","Radeon FG selection survived migration")
                }
                root.page="pro"; veyra.muted=true
                veyra.nrEnabled=false; veyra.srEnabled=false
                if (test.combined) {
                    var preset=named(root.contentItem,"radeon-nr-fg-preset")
                    check(preset!==null,"Radeon preset button missing")
                    if(test.gpuSwitch) check(preset.text==="NVIDIA NR + DLSS-FG" && preset.enabled,"GPU-aware preset label/state incorrect")
                    preset.clicked()
                    if(test.fast60) {
                        veyra.nrLayers.forEach(function(layer) { veyra.setNrLayerParameter(layer.index,"sizePolicy",3) })
                        veyra.videoSrQuality=5; veyra.srTargetIndex=2; veyra.srEnabled=true
                    }
                }
                check(veyra.addEffect(test.vfgHigh ? "vfg-fg" : veyra.amdNrGpu ? "anr-fg" : "dlss-fg") >= 0,"Requested FG backend unavailable")
                if(test.vfgHigh) {
                    veyra.vfgQuality=2
                    veyra.vfgAutoRegulate=false
                    veyra.fgAutoMultiplierEnabled=false
                }
                veyra.fgMultiplier=2
                if(test.menuControls) {
                    var nrCard=named(root.contentItem,"list-nr-stage")
                    var hdrCard=named(root.contentItem,"list-video-hdr")
                    check(nrCard && hdrCard,"NR/HDR cards missing")
                    nrCard.toggled(false); hdrCard.toggled(false)
                    check(nrCard.switchAvailable && hdrCard.switchAvailable,"NR/HDR cards incorrectly unavailable")
                    var nrSwitch=named(root.contentItem,"list-nr-master")
                    check(nrSwitch && nrSwitch.enabled,"NR master switch disabled while off")
                    nrSwitch.toggled(true); hdrCard.toggled(true)
                    check(veyra.nrAnyEnabled && veyra.videoHdr,"NR/HDR toggles did not reach player")
                    var srCard=named(root.contentItem,"list-sr")
                    srCard.toggled(false)
                    check(srCard.switchAvailable,"SR card incorrectly unavailable while off")
                    srCard.toggled(true)
                    check(veyra.srEnabled,"SR toggle did not reach player")
                    var layerCard=named(root.contentItem,"nr-card-1")
                    check(layerCard && layerCard.switchAvailable,"NR layer switch unavailable")
                    var fgSwitch=named(root.contentItem,"list-fg-enabled")
                    veyra.fgEnabled=false
                    check(fgSwitch && fgSwitch.enabled,"FG switch disabled while off")
                    fgSwitch.toggled(true)
                    check(veyra.fgEnabled,"FG toggle did not reach player")
                    console.log("EFFECT_MENU_CONTROLS_PASS")
                }
                veyra.openPath(test.media)
                test.phase=1
            } else if (test.phase === 1) {
                if (test.ticks > 40 && veyra.running && !veyra.applying && !veyra.fgActive && veyra.fgMultiplier < 2) throw new Error("FG rejected: "+veyra.runStatusDetail)
                if (veyra.running && veyra.fgActive && !veyra.applying) {
                    ++test.activeTicks
                    if(test.vfgHigh && test.activeTicks>=8) {
                        check(veyra.fgBackendName==="vfg" && veyra.vfgQuality===2 && !veyra.vfgAutoRegulate,"VFG High setting did not remain active")
                        if(test.activeTicks===8) console.log("NVIDIA_VFG_HIGH_ACTIVE",JSON.stringify({gpu:veyra.gpuMonitorName,layers:veyra.nrLayers,status:veyra.runStatusDetail}))
                    }
                    if(test.combined && test.activeTicks>=8) {
                        check(veyra.outputSummary==="3840x2160","4K output missing: "+veyra.outputSummary)
                        check(veyra.nrEnabled && veyra.nrActive && veyra.stageTimings.some(function(t) { return t.label==="NR" && t.measured && t.ms>0 }),"NR was not active/measured")
                        if(test.menuControls) {
                            check(veyra.videoHdr,"HDR setting was lost")
                            var hdrMeasured=veyra.stageTimings.some(function(t) { return t.label==="HDR" && t.measured && t.ms>0 })
                            if(veyra.displayHdr===1) check(hdrMeasured,"HDR display is enabled but conversion not measured")
                            else {
                                check(veyra.displayHdr===0 && !hdrMeasured && veyra.videoHdrStatus.indexOf("SDR")>=0,"SDR display should explicitly report HDR bypass")
                            }
                            if(test.activeTicks===8) console.log("HDR_DISPLAY_CONTRACT",JSON.stringify({displayHdr:veyra.displayHdr,status:veyra.videoHdrStatus,measured:hdrMeasured}))
                        }
                    }
                    if (test.activeTicks % 8 === 0) console.log("RADEON_DLSS_FG_SAMPLE",JSON.stringify({fps:veyra.submitFps,position:veyra.position,status:veyra.runStatusDetail,output:veyra.outputSummary,nr:veyra.nrEnabled,timings:veyra.stageTimings}))
                    if (test.activeTicks === 8 && test.lifecycle && test.cycle===0) { veyra.startRecording(); veyra.takeScreenshot() }
                    if (test.activeTicks >= 32) {
                        if(!test.lifecycle || test.cycle===2) { console.log("RADEON_DLSS_FG_PASS"); clock.stop(); veyra.stopPlayback(); Qt.quit() }
                        else if(test.cycle===0) { veyra.stopRecording(); veyra.fgEnabled=false; test.phase=2; test.phaseTicks=0 }
                        else { veyra.stopPlayback(); test.phase=5; test.phaseTicks=0 }
                    }
                }
            } else if (test.phase===2 && ++test.phaseTicks>8 && !veyra.applying) {
                check(!veyra.fgActive,"FG stayed active after switch off")
                veyra.fgEnabled=true; veyra.seekTo(20); test.phase=3; test.phaseTicks=0
            } else if (test.phase===3 && ++test.phaseTicks>12 && !veyra.applying && veyra.fgActive) {
                veyra.togglePlayPause(); test.phase=4; test.phaseTicks=0
            } else if (test.phase===4 && ++test.phaseTicks>4) {
                check(veyra.paused,"pause failed"); veyra.togglePlayPause()
                test.cycle=1; test.activeTicks=0; test.phase=1
            } else if (test.phase===5 && ++test.phaseTicks>8 && !veyra.running) {
                veyra.openPath(test.media); test.cycle=2; test.activeTicks=0; test.phase=1
            }
        } catch(error) { console.error("RADEON_DLSS_FG_FAIL",error.message); clock.stop(); veyra.stopPlayback(); Qt.quit() } }
    }
}
