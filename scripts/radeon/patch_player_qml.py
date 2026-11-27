"""Apply reviewable QML changes only to the private, unmodified 2.2 player copy."""
from pathlib import Path
import hashlib
import json

HERE=Path(__file__).resolve().parent

def patch(app, donor):
    qml=app/'qml/Veyra'; base=donor/'versions/2.2.0/qml/Veyra'
    manifest=[]
    for name in ('EffectSupport.js','ProPage.qml','NodePage.qml','SavePresetDialog.qml','ProStatusLight.qml','Main.qml'):
        source=(base/name).read_text(encoding='utf-8')
        text=source
        if name=='EffectSupport.js':
            text=text.replace('return "ANR FG"','return "DLSS-FG · Radeon"')
            text+='\n'+(HERE/'player/Integration.js').read_text(encoding='utf-8')
        else:
            if 'import "EffectSupport.js" as Support' not in text:
                text='import "EffectSupport.js" as Support\n'+text
            text=text.replace('veyra.fgBackendChoices','Support.radeonChoices(veyra)')
            text=text.replace('veyra.fgProviderText','Support.radeonHint(veyra)')
            text=text.replace('veyra.fgMultiplierChoices','Support.radeonMultipliers(veyra)')
            text=text.replace('veyra.effectCatalog','Support.radeonEffects(veyra)')
            # Preserve the length guard; translate only the displayed detail.
            text=text.replace('" · " + veyra.runStatusDetail','" · " + Support.radeonText(veyra.runStatusDetail)')
            if name in ('ProPage.qml','NodePage.qml'):
                text=text.replace('text: modelData.label','text: Support.radeonLabel(modelData.label)')
                text=text.replace('text: card.node.label || ""','text: Support.radeonLabel(card.node.label || "")')
                text=text.replace('title: targetNode.label || qsTr("节点")','title: Support.radeonLabel(targetNode.label || qsTr("节点"))')
                text=text.replace('ToolTip.text: modelData.label','ToolTip.text: Support.radeonLabel(modelData.label)')
            if name=='ProPage.qml':
                anchor='            // View toggle. Choosing '
                assert text.count(anchor)==1
                button='''            VButton {
                objectName: "radeon-nr-fg-preset"
                text: Support.gpuPresetName(veyra)
                enabled: Support.preferredNr(veyra) >= 0 && Support.gpuCapabilityAvailable(veyra, veyra.amdNrGpu ? "anr" : "dlss")
                tip: veyra.uiLanguage === "ko" ? "NR 화질 개선 + 4K 출력 + DLSS 프레임 생성 2X" : "NR enhancement + 4K output + DLSS Frame Generation 2X"
                onClicked: Support.applyRadeon(veyra)
            }

'''
                text=text.replace(anchor,button+anchor)
            if name=='Main.qml':
                pos=text.rfind('}')
                text=text[:pos]+'\n    Timer { interval: 750; running: true; repeat: false; onTriggered: Support.reconcileGpu(veyra) }\n'+text[pos:]
        assert text!=source
        target=qml/name
        target.write_text(text,encoding='utf-8')
        manifest.append(dict(file=name,sha256=hashlib.sha256(target.read_bytes()).hexdigest()))
    (app/'radeon-qml-manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
