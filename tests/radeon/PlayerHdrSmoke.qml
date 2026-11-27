import QtQuick
Item {
    id: test
    property string media: ""
    property int ticks: 0
    property int phase: 0
    property int stableTicks: 0
    function check(ok, reason) { if (!ok) throw new Error(reason) }
    function named(item,name) {
        if(item.objectName===name) return item
        var children=item.children || []
        for(var i=0;i<children.length;++i) { var found=named(children[i],name); if(found) return found }
        return null
    }
    function measured(label) {
        return veyra.stageTimings.some(function(t) { return t.label===label && t.measured && t.ms>0 })
    }
    function sample() {
        console.log("RADEON_HDR_SAMPLE",JSON.stringify({phase:phase,output:veyra.outputSummary,
            hdr:veyra.videoHdr,status:veyra.videoHdrStatus,display:veyra.displayHdr,
            fg:veyra.fgActive,nr:veyra.nrActive,fps:veyra.submitFps,timings:veyra.stageTimings}))
    }
    Timer { id: clock; interval: 250; repeat: true; running: test.media !== ""
        onTriggered: { try {
            if (++test.ticks > 560) throw new Error("140s deadline: " + veyra.runStatusDetail)
            if(test.ticks===2) {
                console.log("RADEON_HDR_SCREENS",JSON.stringify(Qt.application.screens.map(function(s) {
                    return {name:s.name,x:s.virtualX,y:s.virtualY,width:s.width,height:s.height,hz:s.refreshRate}
                })))
                // This machine's 240 Hz display is the enabled HDR output.
                var screen=Qt.application.screens.filter(function(s) { return s.name==="LG ULTRAGEAR+" })[0]
                check(screen!==undefined,"240 Hz HDR test display missing")
                root.screen=screen;root.x=screen.virtualX+50;root.y=screen.virtualY+50
            }
            if (test.phase===0 && test.ticks>=8) {
                root.page="pro";veyra.muted=true
                var preset=named(root.contentItem,"radeon-nr-fg-preset")
                check(preset!==null,"Radeon preset missing");preset.clicked()
                check(veyra.amdNrGpu,"Test requires Radeon")
                check(veyra.effectCapabilities.hdr.available,"HDR capability not available")
                check(veyra.addEffect("anr-fg")>=0,"RIFE unavailable")
                veyra.fgMultiplier=2;veyra.videoHdr=true
                check(veyra.setVideoHdrParameter("middleGray",44),"middle gray rejected")
                check(veyra.setVideoHdrParameter("peakNits",1000),"peak rejected")
                veyra.openPath(test.media);test.phase=1
            } else if(test.phase>0 && test.phase<6) {
                if(!veyra.running || veyra.applying) { test.stableTicks=0;return }
                ++test.stableTicks
                if(test.stableTicks<32)return
                check(veyra.displayHdr===1,"Windows HDR is not enabled on this display: "+veyra.displayHdr)
                check(veyra.outputSummary==="3840x2160","4K output missing")
                check(veyra.nrActive && measured("NR"),"NR inactive")
                if(test.phase===2) check(!measured("HDR"),"HDR stayed active after toggle off")
                else check(veyra.videoHdr && measured("HDR"),"HDR not measured")
                check(veyra.fgActive===(test.phase!==4),"Unexpected FG active state")
                test.sample();test.stableTicks=0
                if(test.phase===1)veyra.videoHdr=false
                if(test.phase===2)veyra.videoHdr=true
                if(test.phase===3)veyra.fgEnabled=false
                if(test.phase===4) { veyra.fgEnabled=true;veyra.seekTo(2) }
                if(test.phase===5)veyra.togglePlayPause()
                ++test.phase
            } else if(test.phase===6 && ++test.stableTicks>6) {
                check(veyra.paused,"Pause failed");veyra.togglePlayPause()
                test.phase=7;test.stableTicks=0
            } else if(test.phase===7 && ++test.stableTicks>32 && veyra.running && !veyra.applying) {
                check(!veyra.paused && veyra.fgActive && measured("HDR"),"HDR/FG did not recover after pause")
                test.sample();console.log("RADEON_HDR_PASS");clock.stop();veyra.stopPlayback();Qt.quit()
            }
        } catch(error) { console.error("RADEON_HDR_FAIL",error.message);clock.stop();veyra.stopPlayback();Qt.quit() } }
    }
}
