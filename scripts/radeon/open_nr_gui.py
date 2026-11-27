# SPDX-License-Identifier: GPL-3.0-only
"""Double-click frontend for the isolated OpenNR video-file probe."""
import argparse
import ctypes
import datetime as dt
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
import traceback
import nr_lab

TEXT = {
    "title": ("Veyra OpenNR · 영상 파일 시험판", "Veyra OpenNR · Video file preview"),
    "scope": ("새 NR 엔진을 영상 파일로 시험합니다. PS Remote 실시간 연결·Veyra 본체 통합은 아직 지원하지 않습니다.", "Test the new NR engine with video files. Live PS Remote and integration into the Veyra player are not available yet."),
    "choose": ("영상 선택…", "Choose video…"), "sample": ("샘플 영상 선택", "Use sample video"),
    "file": ("입력 영상", "Input video"), "output": ("출력 크기", "Output size"),
    "model": ("내부 추론 비율", "Internal model scale"), "seconds": ("시험 구간 (초)", "Duration (sec)"),
    "strength": ("NR 강도", "NR intensity"), "seconds_unit": ("초", "seconds"),
    "hint": ("선택한 영상의 처음 구간만 처리합니다. SDR 전용·원본 프레임률 최대 60fps·오디오 제외. 작은 입력의 4K 확대에는 일반 영상 스케일러를 사용합니다.", "Processes the beginning of the selected clip. SDR only, source frame rate up to 60fps, no audio. Smaller inputs use a conventional scaler for 4K output."),
    "start": ("NR 처리 시작", "Start NR processing"), "cancel": ("중지", "Stop"),
    "folder": ("결과 폴더", "Results folder"), "play": ("비교 영상 재생", "Play comparison"),
    "ready": ("영상을 선택하고 ‘NR 처리 시작’을 누르세요.", "Choose a video and click Start NR processing."),
    "running": ("처리 중… 첫 실행은 모델 준비에 시간이 걸릴 수 있습니다.", "Processing… The first run may take time to prepare the model."),
    "error": ("실행 실패", "Processing failed"), "choose_error": ("먼저 영상 파일을 선택해 주세요.", "Please select a video file first."),
    "stopped": ("중지했습니다. 로그와 임시 파일은 결과 폴더에 보관했습니다.", "Stopped. Logs and temporary files are kept in the results folder."),
    "before": ("원본 · 첫 프레임", "Original · first frame"), "after": ("NR 결과 · 첫 프레임", "NR result · first frame"),
    "done": ("완료: {frames}프레임 / {w}×{h} / 내부 추론 비율 {scale:.0%}. 화면 표시 FPS는 측정하지 않았습니다.", "Complete: {frames} frames / {w}×{h} / model scale {scale:.0%}. Displayed FPS was not measured."),
    "failed_hint": ("실행에 실패했습니다. 아래 로그에서 원인을 확인할 수 있습니다.", "Processing failed. See the log below for details."),
    "close": ("처리 중인 시험만 중지하고 창을 닫을까요?", "Stop this test and close the window?"),
    "missing": ("실행 파일 또는 모델을 찾을 수 없습니다. 설정 파일: ", "Engine or model not found. Configuration file: "),
    "logs": ("진행 기록", "Progress log"), "cancelled": ("중지 요청 중…", "Stopping…"),
    "video_types": ("영상 파일", "Video files"), "all_types": ("모든 파일", "All files"),
    "engine": ("엔진: 소스에서 빌드한 OpenNR · NVIDIA Small 모델 · Radeon", "Engine: source-built OpenNR · NVIDIA Small model · Radeon"),
}
PHASES = {
    "입력 확인": "Inspecting input", "원본 프레임 준비": "Preparing input frames",
    "Radeon 뉴럴렌더링 실행": "Running Radeon neural rendering",
    "before 이미지 저장": "Saving original frame", "after 이미지 저장": "Saving NR frame",
    "미리보기 준비": "Preparing preview", "NR 영상 저장 (오디오 제외)": "Saving NR video (no audio)",
    "비교 영상 저장 (왼쪽 원본 / 오른쪽 NR)": "Saving comparison (original left / NR right)", "완료": "Complete",
}


class Cancelled(Exception): pass


class Runner:
    def __init__(self):
        self.cancelled = threading.Event()

    def run(self, args, *, capture_output, timeout, **kwargs):
        if self.cancelled.is_set(): raise Cancelled()
        started = time.monotonic()
        with subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kwargs) as child:
            while True:
                try:
                    stdout, stderr = child.communicate(timeout=.2)
                    if self.cancelled.is_set(): raise Cancelled()
                    return subprocess.CompletedProcess(args, child.returncode, stdout, stderr)
                except subprocess.TimeoutExpired:
                    if self.cancelled.is_set() or time.monotonic()-started > timeout:
                        child.terminate()
                        try: child.communicate(timeout=3)
                        except subprocess.TimeoutExpired:
                            child.kill(); child.communicate()
                        if self.cancelled.is_set(): raise Cancelled()
                        raise RuntimeError(f"Process exceeded {timeout}s: {Path(args[0]).name}")


class App:
    def __init__(self, home):
        self.home=home; self.config=json.loads((home/"launcher.json").read_text(encoding="utf-8"))
        self.engine=Path(self.config["engineHome"]); self.runner=None; self.busy=False
        self.events=queue.Queue(); self.result=None; self.images={}; self.widgets=[]; self.status_key="ready"; self.last_report=None
        self.root=tk.Tk(); self.root.geometry("1150x850"); self.root.minsize(920,700)
        self.root.configure(bg="#161820")
        style=ttk.Style(); style.theme_use("clam")
        style.configure(".",background="#20232d",foreground="#eeeeee",font=("Segoe UI",11))
        style.configure("TButton",padding=8); style.map("TButton",background=[("active","#414960")])
        style.configure("TEntry",fieldbackground="#313644",foreground="#ffffff")
        style.configure("TCombobox",fieldbackground="#313644",foreground="#ffffff")
        style.map("TCombobox",fieldbackground=[("readonly","#313644")],foreground=[("readonly","#ffffff")])
        self.root.option_add("*TCombobox*Listbox.background","#313644")
        self.root.option_add("*TCombobox*Listbox.foreground","#ffffff")
        panel=ttk.Frame(self.root,padding=20); panel.pack(fill="both",expand=True)
        top=ttk.Frame(panel);top.pack(fill="x")
        self.lang=tk.StringVar(value="한국어")
        language=ttk.Combobox(top,textvariable=self.lang,values=("한국어","English"),state="readonly",width=10)
        language.pack(side="right");language.bind("<<ComboboxSelected>>",lambda _:self.translate())
        self.label(top,"title",font=("Segoe UI",22,"bold")).pack(side="left")
        self.label(panel,"scope",wraplength=1090).pack(anchor="w",pady=(8,5))
        self.label(panel,"engine").pack(anchor="w",pady=(0,15))
        row=ttk.Frame(panel);row.pack(fill="x")
        self.path=tk.StringVar();ttk.Entry(row,textvariable=self.path).pack(side="left",expand=True,fill="x")
        self.button(row,"choose",self.choose).pack(side="left",padx=6)
        self.button(row,"sample",self.sample).pack(side="left")
        options=ttk.Frame(panel);options.pack(fill="x",pady=14)
        self.size=tk.StringVar(value="4K (3840×2160)"); self.scale=tk.StringVar(value="67%")
        self.seconds=tk.StringVar(value="1"); self.strength=tk.StringVar(value="1.0")
        for key,var,values,width in (("output",self.size,("1080p (1920×1080)","4K (3840×2160)"),21),
                ("model",self.scale,("50%","67%","100%"),6), ("seconds",self.seconds,("1","2","3"),4),
                ("strength",self.strength,("0.0","0.5","1.0"),5)):
            self.label(options,key).pack(side="left",padx=(0,6))
            ttk.Combobox(options,textvariable=var,values=values,width=width,state="readonly").pack(side="left",padx=(0,16))
        self.label(panel,"hint",wraplength=1090).pack(anchor="w")
        buttons=ttk.Frame(panel);buttons.pack(fill="x",pady=12)
        self.start_button=self.button(buttons,"start",self.start);self.start_button.pack(side="left")
        self.stop_button=self.button(buttons,"cancel",self.stop);self.stop_button.pack(side="left",padx=6);self.stop_button.configure(state="disabled")
        self.folder_button=self.button(buttons,"folder",self.open_folder);self.folder_button.pack(side="left",padx=6)
        self.play_button=self.button(buttons,"play",self.play);self.play_button.pack(side="left");self.play_button.configure(state="disabled")
        self.status=tk.StringVar();ttk.Label(panel,textvariable=self.status,wraplength=1090).pack(anchor="w",pady=6)
        self.progress=ttk.Progressbar(panel,mode="indeterminate");self.progress.pack(fill="x",pady=(0,8))
        previews=ttk.Frame(panel);previews.pack(fill="both",expand=True)
        self.canvases={}
        for key in ("before","after"):
            block=ttk.Frame(previews);block.pack(side="left",fill="both",expand=True,padx=(0,6))
            self.label(block,key).pack(anchor="w")
            canvas=tk.Canvas(block,width=520,height=293,bg="#0c0d12",highlightthickness=0)
            canvas.pack(fill="both",expand=True);self.canvases[key]=canvas
        self.label(panel,"logs").pack(anchor="w",pady=(10,3))
        self.log=tk.Text(panel,height=5,bg="#101218",fg="#c4cedf",font=("Consolas",10),wrap="word",state="disabled")
        self.log.pack(fill="x")
        self.root.protocol("WM_DELETE_WINDOW",self.close)
        self.translate();self.poll()
        self.root.after(250,lambda:(self.root.lift(),self.root.focus_force()))

    def text(self,key): return TEXT[key][1 if self.lang.get()=="English" else 0]
    def label(self,parent,key,**kw):
        widget=ttk.Label(parent,**kw);self.widgets.append((widget,key));return widget
    def button(self,parent,key,fn):
        widget=ttk.Button(parent,command=fn);self.widgets.append((widget,key));return widget
    def translate(self):
        for widget,key in self.widgets:widget.configure(text=self.text(key))
        self.root.title(self.text("title"))
        if self.status_key:self.status.set(self.text(self.status_key))
        elif self.last_report:self.show_done(self.last_report)
    def show_done(self,report):
        self.status.set(self.text("done").format(frames=report["framesCompleted"],w=report["width"],h=report["height"],scale=report["modelScale"]))
    def append(self,text):
        self.log.configure(state="normal");self.log.insert("end",text+"\n");self.log.see("end");self.log.configure(state="disabled")
    def choose(self):
        path=filedialog.askopenfilename(title=self.text("file"),filetypes=[(self.text("video_types"),"*.mp4 *.mkv *.mov *.avi *.webm"),(self.text("all_types"),"*.*")])
        if path:self.path.set(path)
    def sample(self):self.path.set(self.config["sampleVideo"])
    def stop(self):
        if self.runner:self.runner.cancelled.set();self.status_key="cancelled";self.status.set(self.text("cancelled"))
    def close(self):
        if self.busy:
            if messagebox.askyesno(self.text("title"),self.text("close")):
                self.stop();self.root.after(100,self.close_when_done)
        else:self.root.destroy()
    def close_when_done(self):
        if self.busy:self.root.after(100,self.close_when_done)
        else:self.root.destroy()
    def open_folder(self):
        path=self.result or self.home/"results";path.mkdir(parents=True,exist_ok=True);os.startfile(path)
    def play(self):
        if self.result and (self.result/"compare.mp4").is_file():os.startfile(self.result/"compare.mp4")
    def start(self):
        if self.busy:return
        source=Path(self.path.get())
        if not source.is_file():messagebox.showerror(self.text("error"),self.text("choose_error"));return
        height=2160 if self.size.get().startswith("4K") else 1080
        scale={"50%":.5,"67%":2/3,"100%":1.}[self.scale.get()]
        seconds=float(self.seconds.get());strength=float(self.strength.get())
        self.runner=Runner();self.busy=True;self.result=self.home/"results"/dt.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        self.start_button.configure(state="disabled");self.stop_button.configure(state="normal");self.play_button.configure(state="disabled")
        self.progress.start(12);self.status_key="running";self.status.set(self.text("running"));self.append(str(self.result))
        def worker():
            try:
                result=nr_lab.process(source,self.engine,self.result,height=height,seconds=seconds,strength=strength,
                    backend="open-nr",model_scale=scale,fps=None,runner=self.runner,
                    media_tools=self.config.get("mediaTools"),
                    progress=lambda phase:self.events.put(("phase",phase)))
                self.events.put(("done",result))
            except Cancelled:self.events.put(("stopped",None))
            except Exception as e:
                self.result.mkdir(parents=True,exist_ok=True)
                (self.result/"error.txt").write_text(traceback.format_exc(),encoding="utf-8")
                self.events.put(("error",str(e)))
        threading.Thread(target=worker,daemon=True).start()
    def poll(self):
        try:
            while True:
                kind,value=self.events.get_nowait()
                if kind=="phase":
                    self.append(PHASES.get(value,value) if self.lang.get()=="English" else value)
                    continue
                self.busy=False;self.runner=None;self.progress.stop()
                self.start_button.configure(state="normal");self.stop_button.configure(state="disabled")
                if kind=="done":
                    folder,report=value;self.status_key=None;self.last_report=report;self.show_done(report)
                    for key in ("before","after"):
                        img=tk.PhotoImage(file=str(folder/(key+"-preview.png"))).subsample(2)
                        self.images[key]=img;canvas=self.canvases[key];canvas.delete("all")
                        canvas.create_image(0,0,anchor="nw",image=img)
                    self.play_button.configure(state="normal" if (folder/"compare.mp4").exists() else "disabled")
                elif kind=="stopped":self.status_key="stopped";self.status.set(self.text("stopped"))
                else:
                    self.status_key="failed_hint";self.status.set(self.text("failed_hint"));self.append(value)
                    messagebox.showerror(self.text("error"),value)
        except queue.Empty:pass
        self.root.after(100,self.poll)


def main():
    p=argparse.ArgumentParser();p.add_argument("--home",type=Path,default=Path(__file__).resolve().parent);a=p.parse_args()
    ctypes.windll.shcore.SetProcessDpiAwareness(1)
    app=App(a.home.resolve());app.root.mainloop()


if __name__=="__main__":
    try: main()
    except Exception:
        detail=traceback.format_exc()
        folder=Path(__file__).resolve().parent
        (folder/"startup-error.log").write_text(detail,encoding="utf-8")
        ctypes.windll.user32.MessageBoxW(None,"실행 실패 / Startup failed\n\n"+detail,"Veyra OpenNR",0x10)
