# SPDX-License-Identifier: GPL-3.0-only
"""Stage the double-click frontend on this PC without changing Veyra itself."""
import configparser
import json
from pathlib import Path
import shutil
import sys

ROOT=Path(__file__).resolve().parents[2]
ENGINE=Path("C:/capture/evidence/veyra-open-nr")
APP=Path("C:/capture/run/Veyra-OpenNR-Preview")


def main():
    ffmpeg,ffprobe=shutil.which("ffmpeg"),shutil.which("ffprobe")
    if not ffmpeg or not ffprobe:raise SystemExit("FFmpeg/FFprobe missing")
    python=Path(sys.executable).with_name("pythonw.exe")
    if not python.is_file():raise SystemExit("pythonw.exe missing")
    config={"engineHome":str(ENGINE),"sampleVideo":"C:/capture/evidence/radeon-nr/pan-input-final.mp4",
            "mediaTools":{"ffmpeg":ffmpeg,"ffprobe":ffprobe},"livePsRemote":False,"integratedVeyra":False}
    APP.mkdir(parents=True,exist_ok=True)
    # The old probe's no-argument path opens its sibling launcher too.
    for folder in (APP,ENGINE/"build"):
        for name in ("open_nr_gui.py","nr_lab.py"):
            shutil.copy2(ROOT/"scripts/radeon"/name,folder/name)
        (folder/"launcher.json").write_text(json.dumps(config,ensure_ascii=False,indent=2),encoding="utf-8")
        ini=configparser.ConfigParser();ini["Paths"]={"Python":str(python)}
        with (folder/"launcher.ini").open("w",encoding="utf-16") as f:ini.write(f)
        shutil.copy2(ROOT/"LICENSE",folder/"COPYING")
    shutil.copy2(ENGINE/"build/VeyraOpenNR.exe",APP/"VeyraOpenNR.exe")
    (APP/"README.txt").write_text("VeyraOpenNR.exe를 더블클릭하세요.\n영상 선택 또는 샘플 영상 선택 → NR 처리 시작 → 비교 영상 재생.\n\n이 프로그램은 새 NR 엔진의 영상 파일 시험판입니다.\n실시간 게임/PS Remote 연결, 오디오 및 Veyra 본체 통합은 아직 제공하지 않습니다.\n이 PC의 Python, FFmpeg 및 C:/capture/evidence/veyra-open-nr 런타임을 사용합니다.\n",encoding="utf-8")
    print(APP/"VeyraOpenNR.exe")


if __name__=="__main__":main()
