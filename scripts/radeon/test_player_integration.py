"""Run real Veyra/Qt/decoder/graph/FG/present with an isolated test profile."""
import json
import os
from pathlib import Path
import subprocess
import time
import shutil
import msvcrt
import argparse
import hashlib

ROOT=Path(__file__).resolve().parents[2]
APP=Path('C:/capture/run/Veyra-Radeon-NR-FG/versions/2.2.0')
EVIDENCE=Path('C:/capture/evidence/veyra-integration')

def main():
    global APP
    parser=argparse.ArgumentParser();parser.add_argument('--combined',action='store_true');parser.add_argument('--lifecycle',action='store_true');parser.add_argument('--fast60',action='store_true');parser.add_argument('--fps60',action='store_true');parser.add_argument('--gpu-switch',action='store_true');parser.add_argument('--menu-controls',action='store_true');parser.add_argument('--vfg-high',action='store_true');parser.add_argument('--app',type=Path,default=APP);args=parser.parse_args()
    APP=args.app.resolve()
    lock=(EVIDENCE/'player-test.lock').open('a+b')
    lock.seek(0);msvcrt.locking(lock.fileno(),msvcrt.LK_NBLCK,1)
    stamp=time.strftime('%Y%m%d-%H%M%S')
    out=EVIDENCE/stamp;out.mkdir()
    for name in ('profile','temp','qml-cache'): (out/name).mkdir()
    if args.gpu_switch:
        for source in (APP.parents[1]/'runtime_local/user-data-2.0.0').glob('chain-session*'):
            shutil.copy2(source,out/'profile'/source.name)
    (out/'profile/qml-preferences.v1.json').write_text(json.dumps(dict(language='ko',autoResume=False,prewarmEnhancement=False,introMusic=False,recordDir=str(out/'recording'),screenshotDir=str(out/'screenshots'),recordAudio=False,recordFps=60)),encoding='utf-8')
    if args.gpu_switch:
        prefs=out/'profile/qml-preferences.v1.json';values=json.loads(prefs.read_text());values.update(monitorGpu='1002:7550:34901da2',computeOnMonitorGpu=True);prefs.write_text(json.dumps(values),encoding='utf-8')
    media=EVIDENCE/'motion-1080p30.mp4' if args.combined else EVIDENCE/'test-motion.mp4'
    if args.fast60 or args.fps60:media=EVIDENCE/'motion-1080p60.mp4'
    entry=APP/'qml/Veyra/Main.qml';original=entry.read_bytes();text=original.decode('utf-8')
    pos=text.rfind('}')
    assert 'PlayerDlssFgSmoke' not in text, 'Another test modified Main.qml; restore it before proceeding'
    loader='\nLoader { source: '+json.dumps((ROOT/'tests/radeon/PlayerDlssFgSmoke.qml').as_uri())+'; onLoaded: { item.vfgHigh='+str(args.vfg_high).lower()+'; item.menuControls='+str(args.menu_controls).lower()+'; item.gpuSwitch='+str(args.gpu_switch).lower()+'; item.combined='+str(args.combined).lower()+'; item.lifecycle='+str(args.lifecycle).lower()+'; item.fast60='+str(args.fast60).lower()+'; item.media='+json.dumps(media.as_posix())+' } }\n'
    entry.write_text(text[:pos]+loader+text[pos:],encoding='utf-8')
    env=os.environ.copy();env.update(TEMP=str(out/'temp'),TMP=str(out/'temp'),VEYRA_LOG_FILE=str(out/'engine.log'),QML_DISABLE_DISK_CACHE='1',QML_DISK_CACHE_PATH=str(out/'qml-cache'),QT_FORCE_STDERR_LOGGING='1')
    command=[str(APP/'veyra_qml_ui.exe'),'--managed-portable','--data-dir',str(out/'profile'),'--page','pro','--size','1280x800','--exit-after','145000']
    try:
        with (out/'console.log').open('wb') as log:
            result=subprocess.run(command,cwd=APP,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=160,creationflags=subprocess.CREATE_NO_WINDOW)
    finally: entry.write_bytes(original)
    console=(out/'console.log').read_text(encoding='utf-8',errors='replace')
    for name in ('radeon-dlss-fg.log','dlssnr_on_amd.log'):
        if (APP/name).exists():shutil.copy2(APP/name,out/name)
    summary={'exit':result.returncode,'passed':result.returncode==0 and 'RADEON_DLSS_FG_PASS' in console and 'RADEON_DLSS_FG_FAIL' not in console,'path':str(out),
             'bridgeSha256':hashlib.sha256((APP/'runtime/experimental/anr/nvngx_dlssg.dll').read_bytes()).hexdigest(),
             'combined':args.combined,'lifecycle':args.lifecycle,'sourceFps':60 if args.fps60 or args.fast60 else 30,'nr720':args.fast60,'gpuSwitch':args.gpu_switch,'menuControls':args.menu_controls,'vfgHigh':args.vfg_high,'app':str(APP)}
    (out/'result.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
    print(json.dumps(summary));print(console[-1800:])
    if not summary['passed']:raise SystemExit(1)

if __name__=='__main__': main()
