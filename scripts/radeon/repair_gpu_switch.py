"""Add audited NVIDIA payload to the private Veyra package; preserve user data."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import sys
import time
from stage_player_integration import DEST, EVIDENCE

ROOT=Path(__file__).resolve().parents[2]
DONOR=Path('C:/capture/deps/Veyra-2.2.0-NVIDIA-donor/Veyra-2.2.0-NVIDIA-win64-portable/versions/2.2.0')
def sha(path):
    with path.open('rb') as stream: return hashlib.file_digest(stream,'sha256').hexdigest()
def main():
    app=DEST/'versions/2.2.0'
    processes=subprocess.check_output(['powershell','-NoProfile','-Command',"Get-Process veyra_qml_ui -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Path"],text=True).strip()
    assert str(app/'veyra_qml_ui.exe').lower() not in processes.lower(), 'Close this Veyra instance before repair'
    assert sha(app/'veyra_qml_ui.exe')==sha(DONOR/'veyra_qml_ui.exe')
    zip_path=Path('C:/capture/deps/Veyra-2.2.0-NVIDIA-win64-portable.zip')
    assert sha(zip_path)=='fd1235483a6e36b2520966ec161104478cd93186e97af3b0d78d2dfa7b129932'
    out=EVIDENCE/('gpu-switch-'+time.strftime('%Y%m%d-%H%M%S'));out.mkdir()
    for rel in ['runtime_local/user-data-2.0.0','versions/2.2.0/qml/Veyra']:
        shutil.copytree(DEST/rel,out/'backup'/rel)
    for rel in ['winmm.dll','runtime/experimental/anr/nvngx_dlssg.dll','release-runtime-manifest.json','runtime/experimental/release-runtime-manifest.json','radeon-qml-manifest.json']:
        source=app/rel
        if source.exists():
            target=out/'backup/versions/2.2.0'/rel;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
    copied=[]
    for source in DONOR.rglob('*'):
        if not source.is_file():continue
        rel=source.relative_to(DONOR);target=app/rel
        if target.exists():continue
        assert rel.parts[0] in ('runtime','licenses') or str(rel)=='vfg-runtime-manifest.json', rel
        target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
        assert sha(source)==sha(target)
        copied.append(dict(path=rel.as_posix(),sha256=sha(target),size=target.stat().st_size))
    # Preserve both vendors' inventory; these manifests are informational, not locks.
    source_manifest=json.loads((DONOR/'release-runtime-manifest.json').read_text(encoding='utf-8'))
    for rel in ('release-runtime-manifest.json','runtime/experimental/release-runtime-manifest.json'):
        target=app/rel;manifest=json.loads(target.read_text(encoding='utf-8'))
        existing={r['path'] for r in manifest['files']}
        manifest['files'] += [r for r in source_manifest['files'] if r['path'] not in existing]
        manifest.update(gpuPackage='AMD + NVIDIA (private local integration)',excludedOtherVendorOnlyRuntimes=False)
        target.write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
    subprocess.run([sys.executable,str(ROOT/'scripts/radeon/build_player_integration.py')],check=True,timeout=900)
    subprocess.run([sys.executable,str(ROOT/'scripts/radeon/stage_player_integration.py')],check=True,timeout=120)
    loader=app/'winmm.dll'
    if loader.exists():
        assert loader.resolve().is_relative_to(app.resolve())
        assert sha(loader)==sha(app/'runtime/experimental/anr/bootstrap/winmm.dll')
        # Backed up above. Only remove the old auto-loaded copy; the identical
        # DLL is retained in the adapter's explicitly loaded bootstrap folder.
        loader.unlink()
    (out/'repair.json').write_text(json.dumps(dict(copied=copied,backup=str(out/'backup'),
        sourceZipSha256=sha(zip_path),playerSha256=sha(app/'veyra_qml_ui.exe'),
        bridgeSha256=sha(app/'runtime/experimental/anr/nvngx_dlssg.dll'),validationPending=True),indent=2),encoding='utf-8')
    print(out)
if __name__=='__main__':main()
