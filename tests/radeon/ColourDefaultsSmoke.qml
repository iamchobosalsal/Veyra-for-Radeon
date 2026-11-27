import QtQuick
Item {
    id: test
    property string mode: "save"
    property int ticks: 0
    property var panel: null
    property var expectedValues: ({exposure:1.15,contrast:-5,temperature:-28,vibrance:4})
    function check(ok,why) { if(!ok)throw new Error(why) }
    function near(a,b) {return Math.abs(a-b)<.0001}
    function named(item,name) {
        if(item.objectName===name)return item
        var children=item.children||[]
        for(var i=0;i<children.length;++i){var found=named(children[i],name);if(found)return found}
        return null
    }
    function baselineRestored() {
        for(var key in expectedValues)check(near(veyra.colourParameter(key),expectedValues[key]),"reset "+key)
        check(veyra.colorEnabled,"saved master switch lost")
    }
    Timer { interval:250; repeat:true; running:true
        onTriggered: {try {
            ++test.ticks
            if(test.ticks===1) root.visible=false
            if(test.ticks===3) {
                check(!veyra.running,"isolated test must not open capture")
                check(veyra.colourState.index>=0,"missing color node")
                var component=Qt.createComponent("file:///C:/capture/evidence/colour-default-hdr-20261125/player/qml/Veyra/ColourPanel.qml")
                check(component.status===Component.Ready,component.errorString())
                test.panel=component.createObject(test,{api:veyra,width:600})
                check(test.panel!==null,"color panel did not load")
            }
            if(test.ticks===5) {
                if(test.mode==="save") {
                    test.baselineRestored()
                    var save=named(test.panel,"colour-save-default")
                    check(save!==null,"save-default control missing");save.clicked()
                }
                check(veyra.hasColourDefaults,"defaults absent after save/restart")
                for(var key in test.expectedValues) {
                    check(near(veyra.colourDefaultParameter(key),test.expectedValues[key]),"stored default "+key)
                    var slider=named(test.panel,"colour-param-"+key)
                    check(slider!==null && near(slider.defaultValue,test.expectedValues[key]),"slider default "+key)
                }
                check(veyra.setColourParameter("exposure",-.45),"edit exposure")
                check(veyra.setColourParameter("temperature",12),"edit temperature")
                check(veyra.setColourParameter("contrast",30),"edit contrast")
                check(veyra.setColourParameter("vibrance",-7),"edit vibrance")
                veyra.colorEnabled=false
            }
            if(test.ticks===7) {
                var slider=named(test.panel,"colour-param-exposure")
                var reset=named(slider,"vslider-reset")
                check(reset!==null,"individual reset missing");reset.clicked()
                check(near(veyra.colourParameter("exposure"),1.15),"individual reset missed saved exposure")
                check(near(veyra.colourParameter("contrast"),30),"individual reset affected another field")
                named(test.panel,"colour-reset-1").clicked()
                check(near(veyra.colourParameter("temperature"),-28),"group temperature reset")
                check(near(veyra.colourParameter("vibrance"),4),"group vibrance reset")
                check(near(veyra.colourParameter("contrast"),30),"group reset affected other group")
                named(test.panel,"colour-reset-all").clicked()
                test.baselineRestored()
                check(veyra.resetColourParameter("contrast"),"bridge scalar reset")
                console.log("COLOUR_DEFAULTS_PASS",test.mode,JSON.stringify(veyra.colourState))
                // A later adjustment must persist independently from the saved defaults.
                if(test.mode==="save") veyra.setColourParameter("exposure",.35)
                stop();Qt.quit()
            }
            if(test.ticks>40)throw new Error("test deadline")
        }catch(error){console.error("COLOUR_DEFAULTS_FAIL",test.mode,error.message);stop();Qt.quit()} }
    }
}
