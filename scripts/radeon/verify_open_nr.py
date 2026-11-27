"""Offline GPU validation of source-built NR. No capture, game input or settings.

The moving input is a diagnostic fixture, not recorded gameplay. GPU timestamps
cover NR only; readback/completion time and displayed FPS are separate concepts.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import time
from pathlib import Path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--home", type=Path, default=Path("C:/capture/evidence/veyra-open-nr"))
    ap.add_argument("--fixtures", type=Path, default=Path("C:/capture/evidence/radeon-nr"))
    ap.add_argument("--name", default="validation")
    a = ap.parse_args()
    dest = a.home / a.name
    dest.mkdir(parents=True, exist_ok=False)
    (dest / "temp").mkdir()
    env = os.environ.copy()
    env.update(TEMP=str(dest / "temp"), TMP=str(dest / "temp"), VEYRA_LOG_FILE=str(dest / "engine.log"))
    commands = []

    def call(args, name):
        commands.append([str(x) for x in args])
        (dest / "commands.json").write_text(json.dumps(commands, indent=2))
        print(name, "starting", flush=True)
        start = time.monotonic()
        with (dest / (name+".log")).open("w") as log:
            r = subprocess.run(commands[-1], cwd=dest, env=env, stdout=log, stderr=subprocess.STDOUT,
                               creationflags=subprocess.CREATE_NO_WINDOW, timeout=280)
        print(name, "exit", r.returncode, "seconds", round(time.monotonic()-start, 2), flush=True)
        if r.returncode:
            raise RuntimeError(f"{name} failed; see {dest / (name+'.log')}")

    moving = a.fixtures / "smoke/input.rgba"
    full = dest / "moving-4k-input.rgba"
    call([shutil.which("ffmpeg"), "-v", "error", "-f", "rawvideo", "-pixel_format", "rgba", "-video_size", "1280x720",
          "-i", moving, "-vf", "scale=3840:2160:flags=lanczos", "-frames:v", "8", "-f", "rawvideo", "-pix_fmt", "rgba", full], "prepare-4k")
    cases = [("native-4k", full, 3840, 2160, 8, 1, 1),
             ("4k-model1440", full, 3840, 2160, 8, 1, 2/3),
             ("reset-1080", a.fixtures / "scene1080.rgba", 1920, 1080, 6, 1, 1),
             ("zero-720", moving, 1280, 720, 8, 0, 1)]
    results = []
    for name, source, w, h, count, strength, scale in cases:
        output, report = dest / (name+".rgba"), dest / (name+".json")
        call([a.home / "build/veyra_open_nr_probe.exe", a.home / "runtime", source, output, report,
              w, h, count, "small", strength, scale], name)
        data = json.loads(report.read_text())
        chunk = w*h*4
        digests = []
        with output.open("rb") as f:
            while frame := f.read(chunk):
                if len(frame) != chunk:
                    raise RuntimeError("short output frame")
                digests.append(hashlib.sha256(frame).hexdigest())
        data["distinctOutputFrames"] = len(set(digests))
        data["frameSha256"] = digests
        if not data["pass"] or data["framesCompleted"] != count:
            raise RuntimeError(f"{name}: failed output validation")
        gpu = [f["gpuMs"] for f in data["frameResults"] if f["gpuMs"] > 0]
        data["reportedNrGpuMeanMs"] = sum(gpu)/len(gpu) if gpu else None
        if name.startswith("4k") or name == "native-4k":
            if len(set(digests)) != count:
                raise RuntimeError("moving fixture produced duplicate frames")
        if name == "reset-1080":
            data["resetFrameExactMatch"] = digests[0] == digests[count//2]
            if not data["resetFrameExactMatch"]:
                raise RuntimeError("reset did not reproduce the first-frame output")
        if strength == 0:
            data["bypassByteIdentical"] = hashlib.sha256(source.read_bytes()).hexdigest() == hashlib.sha256(output.read_bytes()).hexdigest()
            if not data["bypassByteIdentical"]:
                raise RuntimeError("zero strength changed the source")
        (dest / (name+"-verified.json")).write_text(json.dumps(data, indent=2))
        results.append({"case": name, "report": str(report), "nrGpuMeanMs": data["reportedNrGpuMeanMs"],
                        "distinctOutputFrames": data["distinctOutputFrames"]})
    (dest / "result.json").write_text(json.dumps({"pass": True, "fixture": "synthetic moving and static SDR inputs",
        "displayFps": None, "nativeGameMotion": False, "results": results}, indent=2))


if __name__ == "__main__":
    main()
