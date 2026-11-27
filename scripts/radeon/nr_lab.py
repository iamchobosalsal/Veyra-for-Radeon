# SPDX-License-Identifier: GPL-3.0-only
"""Independent SDR image/video NR lab. Python stdlib + installed FFmpeg only.

Uses either the production LmxxfNrBackend or the isolated source-built OpenNR
probe. Raw buffers and CPU readback are confined to this offline experiment.
"""
from __future__ import annotations
import argparse
import datetime as dt
import json
import math
import os
from pathlib import Path
import queue
import shutil
import subprocess
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

BASE = Path(__file__).resolve().parent
IMAGE_EXTENSIONS = {".png", ".jpg", ".jpeg", ".bmp", ".webp"}


def process(source: Path, home: Path, output: Path, height=720, model="small", seconds=3, strength=1., progress=print,
            *, backend="lmxxf", model_scale=1., fps=30, runner=None, media_tools=None):
    source, home, output = source.resolve(), home.resolve(), output.resolve()
    if not source.is_file():
        raise ValueError("입력 파일을 찾을 수 없습니다.")
    if backend not in ("lmxxf", "open-nr"):
        raise ValueError("Unknown NR backend")
    exe = home / ("build/veyra_open_nr_probe.exe" if backend == "open-nr" else "radeon_nr_probe.exe")
    runtime = home / "runtime"
    model_file = runtime / (f"dlssnr-amd/dlssnr{'-small' if model=='small' else ''}.bin" if backend == "open-nr" else "LmxxfNrRuntime.dll")
    if not exe.is_file() or not model_file.is_file():
        raise ValueError("실행 파일과 runtime 폴더를 찾을 수 없습니다.")
    ffmpeg, ffprobe = ((media_tools.get("ffmpeg"),media_tools.get("ffprobe")) if media_tools else
                      (shutil.which("ffmpeg"),shutil.which("ffprobe")))
    if not ffmpeg or not ffprobe:
        raise ValueError("FFmpeg / FFprobe가 필요합니다.")
    allowed_heights = (720,1080,1440,2160) if backend == "open-nr" else (720,1080)
    if height not in allowed_heights or model not in ("small", "original") or not 0 < seconds <= 10 or not 0 <= strength <= 1 or not .25 <= model_scale <= 1:
        raise ValueError("잘못된 처리 설정입니다.")
    output.mkdir(parents=True, exist_ok=False)
    scratch = output / "temp"
    scratch.mkdir()
    env = os.environ.copy()
    env.update(TEMP=str(scratch), TMP=str(scratch), VEYRA_LOG_FILE=str(output / "engine.log"))
    # The lab honors the runtime's model/config checks, including incompatible
    # externally supplied DLSS5_* overrides. It never changes the system driver.
    creation = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    commands = []

    def call(args, label, timeout=180):
        commands.append([str(a) for a in args])
        progress(label)
        result = (runner.run if runner else subprocess.run)(commands[-1], cwd=output, env=env, capture_output=True, timeout=timeout, creationflags=creation)
        (output / "commands.json").write_text(json.dumps(commands, ensure_ascii=False, indent=2), encoding="utf-8")
        (output / f"{len(commands):02d}.log").write_bytes(result.stdout + result.stderr)
        if result.returncode:
            detail = (result.stdout + result.stderr).decode("utf-8", errors="replace")[-2500:]
            raise RuntimeError(f"{label} 실패 ({result.returncode})\n{detail}")
        return result.stdout

    meta = json.loads(call([ffprobe, "-v", "error", "-select_streams", "v:0", "-show_streams", "-of", "json", source], "입력 확인"))
    if not meta.get("streams"):
        raise ValueError("영상 또는 이미지 스트림이 없습니다.")
    stream = meta["streams"][0]
    if (stream.get("color_transfer") in ("smpte2084", "arib-std-b67")
            or stream.get("color_primaries") == "bt2020"
            or stream.get("color_space") in ("bt2020nc", "bt2020c")):
        raise ValueError("이 독립 시제품은 SDR 전용입니다. HDR 입력은 아직 지원하지 않습니다.")
    is_image = source.suffix.lower() in IMAGE_EXTENSIONS
    iw, ih = stream["width"], stream["height"]
    # Aspect-preserving 16:9 bounds; only the OpenNR preview allows enlargement.
    ratio = min(height/ih, (height*16/9)/iw)
    if backend != "open-nr": ratio = min(1.,ratio)
    w, h = max(2, int(iw*ratio)//2*2), max(2, int(ih*ratio)//2*2)
    if fps is None:
        from fractions import Fraction
        try: fps = min(60., max(1.,float(Fraction(stream.get("avg_frame_rate", "30/1")))))
        except (ValueError,ZeroDivisionError): fps = 30
    if not isinstance(fps,(int,float)) or not math.isfinite(fps) or not 1 <= fps <= 60:
        raise ValueError("Invalid test frame rate")
    count = 1 if is_image else max(1, math.floor(seconds*fps))
    required = w*h*4*count*2 + 128*1024*1024
    if shutil.disk_usage(output).free < required:
        raise ValueError("임시 원본/결과 프레임을 저장할 디스크 공간이 부족합니다.")
    filters = []
    color_note = "RGB image treated as sRGB; embedded ICC profiles are not applied."
    if not is_image:
        # Keep input tags when present; only fill missing SDR tags with BT.709.
        tags = {"color_primaries": stream.get("color_primaries", "unknown"), "color_trc": stream.get("color_transfer", "unknown"), "colorspace": stream.get("color_space", "unknown")}
        defaults = [f"{k}=bt709" for k,v in tags.items() if v in ("unknown", "unspecified", "")]
        if defaults:
            filters.append("setparams=" + ":".join(defaults))
        filters += ["colorspace=all=bt709:trc=srgb:format=yuv444p", f"fps={fps}"]
        color_note = f"SDR video converted to sRGB; missing color tags assumed BT.709; sampled at {fps:g} fps."
    filters += [f"scale={w}:{h}:flags=lanczos", "format=rgba"]
    raw_in, raw_out = output/"input.rgba", output/"output.rgba"
    call([ffmpeg, "-hide_banner", "-loglevel", "error", "-i", source, "-an", "-vf", ",".join(filters), "-frames:v", count, "-pix_fmt", "rgba", "-f", "rawvideo", raw_in], "원본 프레임 준비")
    count = raw_in.stat().st_size // (w*h*4)
    if not count:
        raise ValueError("입력 프레임이 없습니다.")
    engine_args = [exe, runtime, raw_in, raw_out, output/"report.json", w, h, count, model, strength]
    if backend == "open-nr": engine_args.extend((model_scale, "--continuous"))
    call(engine_args, "Radeon 뉴럴렌더링 실행", timeout=280)
    report = json.loads((output/"report.json").read_text())
    if not report.get("pass") or report["framesCompleted"] != count or raw_out.stat().st_size != raw_in.stat().st_size:
        raise RuntimeError("결과 프레임 검증에 실패했습니다.")
    raw_args = ["-f", "rawvideo", "-pixel_format", "rgba", "-video_size", f"{w}x{h}", "-framerate", fps]
    output_color = "scale=out_color_matrix=bt709:out_range=tv,setparams=range=limited:color_primaries=bt709:color_trc=iec61966-2-1:colorspace=bt709"
    for name, raw in (("before", raw_in), ("after", raw_out)):
        call([ffmpeg, "-hide_banner", "-loglevel", "error", *raw_args, "-i", raw, "-frames:v", 1, "-update", 1, output/f"{name}.png"], f"{name} 이미지 저장")
        call([ffmpeg, "-hide_banner", "-loglevel", "error", "-i", output/f"{name}.png", "-vf", "scale=960:540:force_original_aspect_ratio=decrease:flags=lanczos", "-frames:v", 1, "-update", 1, output/f"{name}-preview.png"], "미리보기 준비")
    if not is_image:
        call([ffmpeg, "-hide_banner", "-loglevel", "error", *raw_args, "-i", raw_out, "-vf", output_color, "-c:v", "libx264", "-threads", 2, "-preset", "fast", "-crf", 16, "-pix_fmt", "yuv420p", "-color_primaries", "bt709", "-color_trc", "iec61966-2-1", "-colorspace", "bt709", "-movflags", "+faststart", output/"nr.mp4"], "NR 영상 저장 (오디오 제외)")
        call([ffmpeg, "-hide_banner", "-loglevel", "error", *raw_args, "-i", raw_in, *raw_args, "-i", raw_out,
              "-filter_complex", "[0:v]scale=640:-2[a];[1:v]scale=640:-2[b];[a][b]hstack," + output_color, "-c:v", "libx264", "-threads", 2, "-preset", "fast", "-crf", 16, "-pix_fmt", "yuv420p", "-color_primaries", "bt709", "-color_trc", "iec61966-2-1", "-colorspace", "bt709", "-movflags", "+faststart", output/"compare.mp4"], "비교 영상 저장 (왼쪽 원본 / 오른쪽 NR)")
    report.update(source=str(source), sourceWidth=iw, sourceHeight=ih, colorHandling=color_note,
                  audioIncluded=False, realtime=False, testFps=fps, backend=backend, modelScale=model_scale)
    (output/"summary.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    # Only remove the two exact scratch files created by this invocation.
    for raw in (raw_in, raw_out):
        if raw.resolve().parent != output:
            raise RuntimeError("unexpected scratch path")
        raw.unlink()
    progress("완료")
    return output, report


def gui(home: Path, preview: Path | None):
    root = tk.Tk()
    root.title("Radeon NR Lab · 독립 뉴럴렌더링 테스트")
    root.configure(bg="#15171c")
    root.geometry("1010x880")
    style = ttk.Style(root)
    style.theme_use("clam")
    outer = ttk.Frame(root, padding=18)
    outer.pack(fill="both", expand=True)
    ttk.Label(outer, text="Radeon NR Lab", font=("Segoe UI", 22, "bold")).pack(anchor="w")
    ttk.Label(outer, text="이미지·짧은 SDR 영상에 실제 뉴럴렌더링 적용 · RX 9000 · 독립 실험판").pack(anchor="w", pady=(4, 14))
    path = tk.StringVar()
    row = ttk.Frame(outer); row.pack(fill="x")
    ttk.Entry(row, textvariable=path).pack(side="left", fill="x", expand=True)
    def choose():
        value = filedialog.askopenfilename(filetypes=[("이미지 / 영상", "*.png *.jpg *.jpeg *.bmp *.webp *.mp4 *.mkv *.mov *.avi"), ("모든 파일", "*.*")])
        if value: path.set(value)
    ttk.Button(row, text="파일 선택", command=choose).pack(side="right", padx=(8,0))
    options = ttk.Frame(outer); options.pack(fill="x", pady=12)
    model, height = tk.StringVar(value="small"), tk.StringVar(value="720")
    for label, variable, values in (("모델", model, ("small", "original")), ("최대 높이", height, ("720", "1080"))):
        ttk.Label(options, text=label).pack(side="left", padx=(0,8))
        ttk.Combobox(options, textvariable=variable, values=values, state="readonly", width=10).pack(side="left", padx=(0,20))
    ttk.Label(options, text="영상은 처음 3초 · 30fps · 오디오 제외").pack(side="left")
    status=tk.StringVar(value="파일을 고른 뒤 실행하세요. 아래 슬라이더로 원본과 결과를 비교할 수 있습니다.")
    ttk.Label(outer, textvariable=status, wraplength=950).pack(anchor="w", pady=(0,10))
    canvas=tk.Canvas(outer,width=960,height=540,bg="#090a0d",highlightthickness=0)
    canvas.pack()
    images={}; current={}; messages=queue.Queue(); busy=False
    def redraw(value="50"):
        if not images: return
        w,h=images["before"].width(),images["before"].height();split=int(float(value)/100*w)
        frame=images["display"]
        root.tk.call(str(frame),"copy",str(images["before"]),"-from",0,0,w,h)
        if split<w:root.tk.call(str(frame),"copy",str(images["after"]),"-from",split,0,w,h,"-to",split,0)
        canvas.delete("all");canvas.create_image(0,0,anchor="nw",image=frame);canvas.create_line(split,0,split,h,fill="#75e6c8",width=2)
        canvas.create_text(12,12,anchor="nw",text="원본",fill="white",font=("Malgun Gothic",12,"bold"))
        canvas.create_text(w-12,12,anchor="ne",text="NR",fill="white",font=("Segoe UI",12,"bold"))
    slider=ttk.Scale(outer,from_=0,to=100,command=redraw);slider.set(50);slider.pack(fill="x",pady=8)
    def show_result(folder):
        before=tk.PhotoImage(file=str(folder/"before-preview.png"));after=tk.PhotoImage(file=str(folder/"after-preview.png"))
        images.update(before=before,after=after,display=tk.PhotoImage(width=before.width(),height=before.height()))
        canvas.configure(height=before.height());current["path"]=folder;redraw(slider.get())
    def work():
        nonlocal busy
        if busy:return
        source=Path(path.get()); selected_model=model.get();selected_height=int(height.get())
        if not source.is_file():messagebox.showerror("파일 선택","이미지 또는 영상을 선택해 주세요.");return
        busy=True;run_button.configure(state="disabled")
        stamp=dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        def background():
            try:messages.put(("done",process(source,home,home/"results"/stamp,selected_height,selected_model,progress=lambda s:messages.put(("status",s)))))
            except Exception as error:messages.put(("error",str(error)))
        threading.Thread(target=background,daemon=True).start()
    buttons=ttk.Frame(outer);buttons.pack(fill="x")
    run_button=ttk.Button(buttons,text="뉴럴렌더링 실행",command=work);run_button.pack(side="left")
    ttk.Button(buttons,text="결과 폴더 열기",command=lambda:os.startfile(current["path"]) if current else None).pack(side="left",padx=8)
    ttk.Button(buttons,text="비교 영상 재생",command=lambda:os.startfile(current["path"]/"compare.mp4") if current and (current["path"]/"compare.mp4").is_file() else None).pack(side="left")
    def poll():
        nonlocal busy
        try:
            while True:
                kind,value=messages.get_nowait()
                if kind=="status":status.set(value)
                elif kind=="done":
                    busy=False;run_button.configure(state="normal");folder,report=value;show_result(folder)
                    status.set(f"완료 · {report['framesCompleted']}프레임 · 결과 변경 {report['changedPixelFraction']:.1%} · 실제 표시 FPS는 측정하지 않았습니다.")
                else:
                    busy=False;run_button.configure(state="normal");status.set("실행 실패 — 로그를 확인하세요.");messagebox.showerror("실행 실패",value)
        except queue.Empty:pass
        root.after(100,poll)
    def close():
        if busy:messagebox.showinfo("처리 중","결과 저장이 끝난 뒤 창을 닫아 주세요.")
        else:root.destroy()
    root.protocol("WM_DELETE_WINDOW",close)
    if preview and (preview/"before-preview.png").is_file():show_result(preview)
    poll();root.mainloop()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--home",type=Path,default=BASE)
    parser.add_argument("--input",type=Path)
    parser.add_argument("--out",type=Path)
    parser.add_argument("--height",type=int,default=720,choices=(720,1080))
    parser.add_argument("--model",default="small",choices=("small","original"))
    parser.add_argument("--seconds",type=float,default=3)
    parser.add_argument("--strength",type=float,default=1.)
    parser.add_argument("--preview",type=Path)
    args=parser.parse_args()
    if args.input:
        output=args.out or args.home/"results"/dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        result,_=process(args.input,args.home,output,args.height,args.model,args.seconds,args.strength)
        print(result)
    else:gui(args.home.resolve(),args.preview or args.home/"demo-off")


if __name__=="__main__":main()
