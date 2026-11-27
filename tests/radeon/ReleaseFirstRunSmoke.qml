import QtQuick
Item {
    id: probe
    property string mode: "cold"
    property string modelFolder: ""
    property string sampleFile: ""
    property int step: 0
    property int ticks: 0
    property int checkpoint: 0
    property var panel: null
    property var expected: ({exposure:0.25,contrast:-1,temperature:-28,vibrance:4})
    function check(ok, why) { if (!ok) throw new Error(why) }
    function near(a,b) { return Math.abs(a-b)<0.0001 }
    function named(item,name) {
        if (item.objectName===name) return item
        var children=item.children||[]
        for(var i=0;i<children.length;i++) {var found=named(children[i],name);if(found)return found}
        return null
    }
    function finish() {
        console.log("RELEASE_UI_PASS="+JSON.stringify({mode:mode,assets:veyra.nrModelAssets,nrActive:veyra.nrActive,nrLayers:veyra.nrLayers,colour:veyra.colourState,timings:veyra.stageTimings}))
        timer.stop();Qt.quit()
    }
    function prepareNr() {
        veyra.fgEnabled=false;veyra.srEnabled=false;veyra.videoHdr=false
        if(veyra.nrLayers.length===0)check(veyra.addEffect("nr")>=0,"add NR")
        var idx=veyra.nrLayers[0].index
        check(veyra.setNrLayerParameter(idx,"runtime",4),"select Radeon runtime")
        check(veyra.setNrLayerParameter(idx,"performance",1),"select Small")
        check(veyra.setNrLayerParameter(idx,"sizePolicy",0),"select 1080p internal NR")
        check(veyra.setEffectEnabled(idx,true),"enable NR")
        check(veyra.nrAnyEnabled,"desired NR remains ON")
    }
    function colourCheck() {
        if(veyra.colourState.index<0)check(veyra.addEffect("color")>=0,"add color node")
        veyra.colorEnabled=true
        for(var key in expected)check(veyra.setColourParameter(key,expected[key]),"set "+key)
        var component=Qt.createComponent("ColourPanel.qml")
        check(component.status===Component.Ready,component.errorString())
        panel=component.createObject(probe,{api:veyra,width:700})
        check(panel!==null,"create actual color panel")
        var save=named(panel,"colour-save-default");check(save!==null,"save defaults control");save.clicked()
        check(veyra.hasColourDefaults,"save defaults")
        check(veyra.setColourParameter("exposure",-0.45),"adjust exposure")
        named(named(panel,"colour-param-exposure"),"vslider-reset").clicked()
        check(near(veyra.colourParameter("exposure"),0.25),"single reset")
        check(veyra.setColourParameter("temperature",12),"adjust temperature")
        named(panel,"colour-reset-1").clicked()
        check(near(veyra.colourParameter("temperature"),-28),"group reset")
        check(veyra.setColourParameter("contrast",30),"adjust contrast")
        named(panel,"colour-reset-all").clicked()
        for(key in expected)check(near(veyra.colourParameter(key),expected[key]),"all reset "+key)
        check(veyra.setColourParameter("exposure",0.35),"persist adjustment independently")
    }
    Timer {
        id:timer; interval:250; repeat:true; running:true
        onTriggered: {try {
            ticks++
            if(ticks>220)throw new Error("deadline step "+step+" assets "+JSON.stringify(veyra.nrModelAssets)+" NR "+veyra.nrActive)
            var a=veyra.nrModelAssets
            if(step===0) {
                if(a.busy||!a.checked)return
                check(a.managed,"expected RX 9000 model management")
                if(mode==="cold") {
                    check(!a.smallReady&&!a.originalReady&&!veyra.effectAvailability("nr4").available,"fresh package must reject missing models")
                    veyra.importNrModelFolder(modelFolder+"/missing-folder");step=1
                } else if(mode==="stale") {
                    check(!a.smallReady&&!veyra.nrModelAvailability(true).available,"stale reference must not fall back")
                    check(veyra.nrAnyEnabled,"startup must preserve desired NR ON when models missing")
                    veyra.openPath(sampleFile);checkpoint=ticks;step=4
                } else {
                    check(a.smallReady&&a.originalReady,"reference restored on restart")
                    check(veyra.nrAnyEnabled,"NR ON restored on restart")
                    check(near(veyra.colourParameter("exposure"),0.35),"color current value persisted")
                    check(near(veyra.colourDefaultParameter("exposure"),0.25),"color saved baseline persisted")
                    veyra.openPath(sampleFile);step=6
                }
            } else if(step===1) {
                if(a.busy)return
                check(a.error.length>0&&!a.smallReady,"invalid folder rejected with reason")
                veyra.importNrModelFolder(modelFolder);step=2
            } else if(step===2) {
                if(a.busy)return
                check(a.smallReady&&a.originalReady&&a.error.length===0,"valid folder ready")
                check(veyra.nrModelAvailability(true).available&&veyra.nrModelAvailability(false).available,"both model mode capabilities")
                colourCheck();prepareNr();checkpoint=ticks;step=3
            } else if(step===3) {
                if(ticks-checkpoint>=5)finish()
            } else if(step===4) {
                if(ticks-checkpoint<12)return
                check(!veyra.nrActive,"missing model cannot run")
                check(veyra.nrAnyEnabled,"failed attempt must preserve desired ON")
                veyra.importNrModelFolder(modelFolder);step=5
            } else if(step===5) {
                if(a.busy)return
                check(a.smallReady&&a.error.length===0&&!a.restartRequired,"failed NR retry accepted without restart")
                step=6
            } else if(step===6) {
                var measured=veyra.stageTimings.some(t=>t.label.indexOf("NR")>=0&&t.measured&&t.samples>0&&t.ms>0)
                if(veyra.nrActive&&measured)finish()
            }
        }catch(e){console.error("RELEASE_UI_FAIL="+JSON.stringify({mode:mode,step:step,error:e.message}));timer.stop();Qt.quit()} }
    }
}
