"""Bounded offline check of frames recorded from Veyra's actual present path."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import statistics
import hashlib
from fractions import Fraction

def main():
    parser=argparse.ArgumentParser();parser.add_argument('recording',type=Path);args=parser.parse_args()
    meta=json.loads(subprocess.check_output([shutil.which('ffprobe'),'-v','error','-show_streams','-show_format','-of','json',str(args.recording)],timeout=30))
    video=next(s for s in meta['streams'] if s['codec_type']=='video')
    raw=subprocess.check_output([shutil.which('ffmpeg'),'-hide_banner','-loglevel','error','-threads','2','-i',str(args.recording),
                                 '-vf','scale=320:180','-frames:v','240','-fps_mode','passthrough','-pix_fmt','rgb24','-f','rawvideo','-'],timeout=90)
    size=180*320*3
    frames=[raw[i:i+size] for i in range(0,len(raw),size)]
    delta=[sum(abs(a-b) for a,b in zip(left,right))/size for left,right in zip(frames,frames[1:])]
    result=dict(width=video['width'],height=video['height'],rate=video['r_frame_rate'],averageFps=float(Fraction(video['avg_frame_rate'])),duration=meta['format']['duration'],
                decodedFrames=len(frames),uniqueFrames=len({hashlib.sha256(f).digest() for f in frames}),
                consecutiveMaeMin=min(delta),consecutiveMaeMedian=statistics.median(delta),
                nearDuplicates=sum(d<.1 for d in delta),pairs=len(delta),
                note='Non-duplicate pixel check; does not certify interpolation accuracy, visual quality or physical scanout.')
    # The live recorder preserves timestamps (VFR). r_frame_rate can be their
    # common denominator, so verify actual average rate, not that nominal field.
    assert video['width']==3840 and video['height']==2160 and abs(result['averageFps']-60)<1
    assert len(frames)>=120 and result['nearDuplicates'] < len(delta)*.1
    args.recording.with_suffix('.verification.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result))
if __name__=='__main__':main()
