"""Build the local FG bridge; NVIDIA headers remain in the external SDK folder."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[2]
COMPILER=Path('C:/capture/deps/llvm-mingw-20261117-ucrt-x86_64/bin/clang++.exe')
SDK=Path('C:/capture/deps/nvidia-dlss-sdk/include')
OUT=Path('C:/capture/evidence/veyra-integration/nvngx_dlssg.dll')
def main():
    OUT.parent.mkdir(parents=True,exist_ok=True)
    subprocess.run([str(COMPILER),'-std=c++20','-O2','-shared','-DUNICODE','-D_UNICODE','-DNOMINMAX','-I',str(SDK),
                    str(ROOT/'src/ngx/RadeonDlssFgBridge.cpp'),str(ROOT/'src/ngx/RadeonDlssFgBridge.def'),
                    '-o',str(OUT),'-static','-ldxgi','-ld3d12','-ldxguid','-luser32'],check=True,timeout=900,cwd=ROOT)
    print(OUT)
if __name__=='__main__':main()
