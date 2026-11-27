"""Install the independent RIFE provider into an existing Veyra local package.

Run only after Veyra has exited. Each replaced file is backed up, and writes
use replacement (never modify a staged hard link in place). No user settings
or NVIDIA native provider are changed. This is a local experimental package.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--app', type=Path, required=True)
    p.add_argument('--built', type=Path, required=True)
    p.add_argument('--model', type=Path, required=True)
    p.add_argument('--backup', type=Path, required=True)
    args = p.parse_args()
    app, built, model, backup = [x.resolve() for x in (args.app, args.built, args.model, args.backup)]
    if not (app / 'veyra_qml_ui.exe').is_file():
        raise ValueError('Expected the Veyra version directory')
    backup.mkdir(parents=True, exist_ok=False)
    fg = Path('runtime/experimental/anr')
    payload = {fg/'nvngx_dlssg.dll': (built/'veyra_rife_fg.dll').read_bytes()}
    for name in ('flownet.bin', 'flownet.param', 'DirectML.dll'):
        payload[fg/name] = (model/name).read_bytes()
    for file in (built/'rife-licenses').iterdir():
        if file.is_file():
            payload[Path('licenses/rife')/file.name] = file.read_bytes()
    support = Path('qml/Veyra/EffectSupport.js')
    source = (app/support).read_text(encoding='utf-8')
    if 'function radeonChoices' not in source:
        raise ValueError('Expected the existing Radeon integration menu')
    text = source.replace('DLSS-FG · Radeon', 'RIFE · Radeon')
    text = text.replace('라데온에서 DLSS 프레임 생성 · 2X', '영상 전용 AI 프레임 생성 · 2X · 실험 기능')
    text = text.replace('DLSS Frame Generation on Radeon · 2X', 'Video AI frame interpolation · 2X · experimental')
    text = text.replace('+ " NR + DLSS-FG"', '+ (api.amdNrGpu ? " NR + RIFE" : " NR + DLSS-FG")')
    payload[support] = text.encode('utf-8')
    # Preserve NVIDIA wording on its own native menu and describe this local
    # preset according to the GPU that is actually selected.
    pro = Path('qml/Veyra/ProPage.qml')
    text = (app/pro).read_text(encoding='utf-8')
    text = text.replace('NR 화질 개선 + 4K 출력 + DLSS 프레임 생성 2X', 'NR 화질 개선 + 4K 출력 + 선택한 GPU의 프레임 생성 2X')
    text = text.replace('NR enhancement + 4K output + DLSS Frame Generation 2X', 'NR enhancement + 4K output + frame generation for the selected GPU')
    payload[pro] = text.encode('utf-8')
    manifest = dict(app=str(app), created=time.strftime('%Y-%m-%dT%H:%M:%S'), provider='RIFE 4.22 Lite / DirectML', files=[])
    # Back up every target before changing any target, including existing
    # model files when updating a previous RIFE installation.
    for rel in payload:
        dest = app/rel
        old = backup/rel
        row = dict(path=rel.as_posix(), existed=dest.exists())
        if dest.exists():
            old.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(dest, old)
            row['beforeSha256'] = digest(old)
        manifest['files'].append(row)
    (backup/'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    applied = []
    try:
        for row, (rel, data) in zip(manifest['files'], payload.items()):
            dest = app/rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            tmp = dest.with_name(dest.name+'.rife-new')
            tmp.write_bytes(data)
            os.replace(tmp, dest)
            applied.append(row)
            row['afterSha256'] = digest(dest)
    except Exception:
        for row in reversed(applied):
            if row['existed']:
                dest = app/row['path']
                tmp = dest.with_name(dest.name+'.rife-restore')
                shutil.copy2(backup/row['path'], tmp)
                os.replace(tmp, dest)
        raise
    (backup/'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    (app/'rife-provider-manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(json.dumps(manifest, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
