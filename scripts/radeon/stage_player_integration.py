"""Stage the real Veyra player with the local Radeon DLSS-FG adapter.

No inference code/model from NeuralHub is redistributed; the user's already
installed loader and runtime are used only in this private local installation.
"""
from pathlib import Path
import hashlib
import json
import shutil
from patch_player_qml import patch

ROOT=Path(__file__).resolve().parents[2]
DONOR=Path('C:/capture/deps/Veyra-2.2.0-AMD-donor/Veyra-2.2.0-AMD-win64-portable')
DEST=Path('C:/capture/run/Veyra-Radeon-NR-FG')
EVIDENCE=Path('C:/capture/evidence/veyra-integration')

def copy_changed(source, target):
    if not target.exists() or hashlib.sha256(source.read_bytes()).digest()!=hashlib.sha256(target.read_bytes()).digest():
        shutil.copy2(source,target)

def main():
    if not DEST.exists(): shutil.copytree(DONOR,DEST,copy_function=shutil.copy2)
    app=DEST/'versions/2.2.0'
    plugin=app/'runtime/experimental/anr'
    plugin.mkdir(parents=True,exist_ok=True)
    copy_changed(EVIDENCE/'nvngx_dlssg.dll',plugin/'nvngx_dlssg.dll')
    bootstrap=plugin/'bootstrap';bootstrap.mkdir(parents=True,exist_ok=True)
    copy_changed(EVIDENCE/'winmm.dll',bootstrap/'winmm.dll')
    copy_changed(EVIDENCE/'neuralhub.ini',bootstrap/'neuralhub.ini')
    copy_changed(EVIDENCE/'neuralhub.ini',app/'neuralhub.ini')
    patch(app,DONOR)
    profile=DEST/'runtime_local/user-data-2.0.0'
    profile.mkdir(parents=True,exist_ok=True)
    prefs=profile/'qml-preferences.v1.json'
    if not prefs.exists():prefs.write_text(json.dumps(dict(language='ko',lastPage='pro',autoResume=False,prewarmEnhancement=False,startupMusicEnabled=False)),encoding='utf-8')
    identity={'player':str(app/'veyra_qml_ui.exe'),
              'playerUnmodified':hashlib.sha256((app/'veyra_qml_ui.exe').read_bytes()).hexdigest()==hashlib.sha256((DONOR/'versions/2.2.0/veyra_qml_ui.exe').read_bytes()).hexdigest(),
              'bridgeSha256':hashlib.sha256((plugin/'nvngx_dlssg.dll').read_bytes()).hexdigest(),
              'nr':'existing lmxxf Radeon NR', 'fg':'installed NeuralHub DLSS-FG through new Veyra adapter',
              'privateLocalInstallation':True,'validationPending':True}
    (EVIDENCE/'player-stage.json').write_text(json.dumps(identity,indent=2),encoding='utf-8')
    print(app)

if __name__=='__main__': main()
