"""Initialize this private package from a passing test, without altering original Veyra."""
import argparse
from pathlib import Path
import hashlib
import json
import shutil
from stage_player_integration import DEST,EVIDENCE

def main():
    parser=argparse.ArgumentParser();parser.add_argument('verified_run',type=Path);args=parser.parse_args()
    verified=args.verified_run.resolve()
    assert verified.is_relative_to(EVIDENCE.resolve())
    result=json.loads((verified/'result.json').read_text())
    bridge=DEST/'versions/2.2.0/runtime/experimental/anr/nvngx_dlssg.dll'
    assert result['passed'] and result['combined'] and not result['nr720']
    assert result['bridgeSha256']==hashlib.sha256(bridge.read_bytes()).hexdigest()
    target=DEST/'runtime_local/user-data-2.0.0'
    original=Path('C:/capture/run/Veyra-2.2.0-original/runtime_local/user-data-2.0.0')
    # Retain input preferences. PS registration is already shared through
    # Veyra's DPAPI-protected LocalAppData store; do not open or copy secrets.
    for name in ('capture-preferences.v1',):
        if (original/name).exists() and not (target/name).exists():shutil.copy2(original/name,target/name)
    # Keep test media/recent-file entries out of the user's startup UI.
    for name in ('chain-session.v1','chain-session.v1.vfg'):
        assert not (target/name).exists(), 'Refuse to overwrite settings after the user starts using this package'
        if (verified/'profile'/name).exists():shutil.copy2(verified/'profile'/name,target/name)
    prefs=json.loads((original/'qml-preferences.v1.json').read_text(encoding='utf-8'))
    prefs.update(language='ko',lastPage='pro',autoResume=False,prewarmEnhancement=False,introMusic=False)
    (target/'qml-preferences.v1.json').write_text(json.dumps(prefs,ensure_ascii=False,indent=2),encoding='utf-8')
    receipt=json.loads((EVIDENCE/'player-stage.json').read_text())
    receipt.update(validationPending=False,verifiedRun=str(verified),bridgeSha256=result['bridgeSha256'],
                   livePsRemoteTested=False,captureHardwareTested=False,
                   lifecycleEvidence=str(EVIDENCE/'20261123-224326'),
                   knownLimitations=['SDR tested only','FSR + reduced NR size can disable SR in upstream 2.2; keep NR at 1080p',
                                     'Estimated video motion and constant depth, not game-engine buffers',
                                     'Module/bootstrap retain one device for process lifetime; restart for GPU changes'])
    (DEST/'radeon-integration.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    (DEST/'시작.txt').write_text('''Veyra Radeon NR + DLSS-FG

실행: 이 폴더의 Veyra.exe
입력: 기존 Veyra처럼 왼쪽 위 소스 메뉴에서 PS5 / 캡처 / 영상 선택
설정: 상단 Radeon NR + DLSS-FG 버튼
기본값: NR 1080p + FSR 4K 출력 + DLSS-FG Radeon 2X
동작 확인: 프레임 생성 탭의 DLSS-FG · Radeon · 처리 중, 하단 NR/FG 처리 시간

설정은 종료 후에도 저장됩니다. NeuralHub 창을 따로 켤 필요는 없습니다.
설치된 NeuralHub 런타임과 가져온 DLL 데이터는 필요합니다.
이 로컬 패키지를 다른 PC에 배포하지 마세요. 외부 런타임은 포함/재배포하지 않았습니다.

검증: RX9070XT, Veyra 실제 디코딩/NR/SR/FG/표시 경로,
1080p30→4K60 및 1080p60→4K 약120 제출fps. 60Hz 모니터는 120Hz가 되지 않습니다.
FG 전환, 탐색, 일시정지, 재생 종료/재시작을 확인했습니다.
PS Remote 실제 접속, 캡처카드, HDR, 장시간 플레이 화질/오디오는 이번 검증에 포함되지 않습니다.
NR 크기를 720p로 낮추면 기존 FSR 경로에서 4K가 해제되는 결함이 있으므로 1080p를 유지하세요.

Starting in English: run Veyra.exe, choose the existing PS5/capture/video input,
then select Radeon NR + DLSS-FG. Change the UI language in Veyra Settings.
''',encoding='utf-8')
    print(DEST/'Veyra.exe')
if __name__=='__main__':main()
