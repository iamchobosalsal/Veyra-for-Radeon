// SPDX-License-Identifier: GPL-3.0-only
// Real-app UI regression driver. The runner supplies only private fixture paths;
// capabilities and button handlers remain the production implementation.
import QtQuick
Item {
    id: probe
    required property var appShell
    required property var pageObject
    property string mode: "cold"
    property string modelFolder: ""
    property int step: 0
    property int ticks: 0
    property int checkpoint: 0
    property var checks: []
    function check(ok,why) { if(!ok)throw new Error(why) }
    function named(item,name) {
        if(!item)return null
        if(item.objectName===name)return item
        var children=item.children||[]
        for(var i=0;i<children.length;i++){var found=named(children[i],name);if(found)return found}
        return null
    }
    function controls(expectMissing,expectBusy) {
        var preset=named(pageObject,"radeon-nr-fg-preset")
        var connect=named(pageObject,"list-nr-model-connect")
        var stage=named(pageObject,"list-nr-stage")
        if(stage&&stage.open!==undefined)stage.open=true
        check(preset&&connect,"actual preset and model connection controls exist")
        check(preset.visible&&connect.visible,"recovery controls visible in actual ProPage")
        check(preset.enabled===!expectBusy&&connect.enabled===!expectBusy,"busy guard on both real controls")
        if(expectMissing&&!expectBusy)check(preset.text==="NR 모델 연결","missing assets show model connection action")
        return {preset:preset,connect:connect}
    }
    function finish() {
        check(!veyra.hasSource&&!veyra.nrActive,"driver must not open a source or infer")
        console.log("NR_CONNECTION_UI_PASS="+JSON.stringify({mode:mode,checks:checks,assets:veyra.nrModelAssets,nrLayers:veyra.nrLayers,nrAnyEnabled:veyra.nrAnyEnabled,fgEnabled:veyra.fgEnabled,fgBackend:veyra.fgBackendName,fgMultiplier:veyra.fgMultiplier,srTargetIndex:veyra.srTargetIndex,noSource:!veyra.hasSource}))
        timer.stop();Qt.quit()
    }
    Timer {
        id:timer;interval:200;repeat:true;running:true
        onTriggered: {try {
            ticks++
            if(ticks>180)throw new Error("deadline step="+step+" assets="+JSON.stringify(veyra.nrModelAssets))
            var a=veyra.nrModelAssets
            if(step===0){
                if(a.busy||!a.checked)return
                check(a.managed&&veyra.amdNrGpu,"actual managed Radeon capabilities required")
                check(!a.smallReady&&!a.originalReady&&veyra.effectAvailability("nr4").modelAssetsMissing,"actual cold profile has no models")
                check(!veyra.hasSource,"no source on cold start")
                check(veyra.setPreference("language","ko"),"select Korean for exact action label")
                veyra.fgEnabled=false;veyra.srEnabled=false;veyra.nrEnabled=false
                if(mode==="legacy")check(veyra.nrLayers.length===3&&veyra.nrLayers.every(function(n){return n.runtime===3}),"legacy 3-layer runtime3 fixture loaded by actual app")
                else {
                    check(mode==="cold","known mode")
                    while(veyra.nrLayers.length)check(veyra.removeEffect(veyra.nrLayers[0].index),"remove default NR node")
                    check(veyra.nrLayers.length===0,"zero NR nodes")
                }
                appShell.goPage("pro");pageObject.tab="quality";checkpoint=ticks;step=1
            }else if(step===1){
                if(ticks-checkpoint<5)return
                controls(true,false)
                checks.push(mode==="legacy"?"legacy-runtime3-three-layer-recovery-visible":"zero-layer-recovery-visible")
                veyra.importNrModelFolder(modelFolder+"/missing-ui-smoke-folder")
                check(veyra.nrModelAssets.busy,"real async model validation started")
                // Let bindings settle before the next timed tick, while the worker is busy.
                Qt.callLater(function(){try{if(veyra.nrModelAssets.busy){controls(true,true);checks.push("busy-controls-disabled")}}catch(e){console.error("NR_CONNECTION_UI_FAIL="+JSON.stringify({mode:mode,step:step,error:e.message}));timer.stop();Qt.quit()}})
                step=2
            }else if(step===2){
                if(a.busy)return
                check(a.error.length>0&&!a.smallReady&&!a.originalReady,"invalid folder reports failure and keeps NR unavailable")
                controls(true,false);checks.push("invalid-folder-remains-recoverable")
                veyra.importNrModelFolder(modelFolder);step=3
            }else if(step===3){
                if(a.busy)return
                check(a.smallReady&&a.originalReady&&a.error.length===0,"actual valid model reference accepted")
                check(veyra.effectAvailability("nr4").available&&veyra.effectCapabilities.anr.available,"real NR and RIFE capability became ready")
                checkpoint=ticks;step=4
            }else if(step===4){
                if(ticks-checkpoint<2)return
                var c=controls(false,false)
                check(c.preset.text!=="NR 모델 연결","ready preset is now apply action")
                c.preset.clicked();checks.push("actual-ready-preset-clicked")
                checkpoint=ticks;step=5
            }else if(step===5){
                if(ticks-checkpoint<3)return
                check(veyra.nrAnyEnabled&&veyra.nrLayers.some(function(n){return n.enabled&&n.runtime===4}),"preset enabled real Radeon NR layer")
                check(veyra.fgEnabled&&veyra.fgBackendName==="anr"&&veyra.fgMultiplier===2,"preset enabled RIFE2X desired settings")
                check(veyra.srTargetIndex===2,"preset selected 4K output")
                checks.push("NR-plus-RIFE2X-applied-no-source");finish()
            }
        }catch(e){console.error("NR_CONNECTION_UI_FAIL="+JSON.stringify({mode:mode,step:step,error:e.message}));timer.stop();Qt.quit()} }
    }
}
