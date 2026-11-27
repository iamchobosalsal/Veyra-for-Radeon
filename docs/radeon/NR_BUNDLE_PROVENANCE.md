# NR 번들 자산 출처

이 포크의 모델 포함 패키지는 원본 Veyra 2.2.0 AMD 공개 ZIP의 NR 자산 640개(872,543,390바이트)를 기반으로 구성합니다. Small·Original 가중치, HIP 커널, codec 셰이더, 모델 메타데이터와 공용 데이터를 함께 보존합니다. 파일을 이름만 보고 생략하지 않습니다.

- 원본: [Veyra-2.2.0-AMD-win64-portable.zip](https://github.com/Likely7/Veyra-NRVideo/releases/download/v2.2.0/Veyra-2.2.0-AMD-win64-portable.zip)
- 원본 검증: [공식 SHA256SUMS.txt](https://github.com/Likely7/Veyra-NRVideo/releases/download/v2.2.0/SHA256SUMS.txt)
- 원본 ZIP SHA256: `b383cd4fbc94a6b84e32f36bc8c2758b4c03216c2bd0a2560e3112ba520d6905`
- 원본 자산 경로: `versions/2.2.0/runtime/amd-nr/assets/`
- 이 배포본 자산 경로: `versions/1.0.0/runtime/amd-nr/assets/`

가중치·커널·셰이더의 바이트를 보존합니다. 설정 파일 `native-game-flags.txt`의 `DLSS5_STYLE`만 이 포크에서 기존에 사용하던 `0`으로 설정합니다. 원본 값은 `1`입니다. 이 선택은 `scripts/amd-nr/stage-044-runtime.py`의 기존 설정과 같습니다. 개인 설정이나 로그를 복사한 것이 아닙니다.

원본의 `LICENSE-lmxxf-MIT.txt`, `HIP-API-LICENSE.txt`, `LOCAL-EXPERIMENT.txt`, `release.json`, `runtime-manifest.json`을 보존합니다. 원본 manifest는 원본 배포 시점의 기록이며, 현재 파일의 해시는 이 포크의 `package-manifest.json`과 `DISTRIBUTION_AUDIT.json`에 기록합니다. 과거 설명이 포함된 원문 고지는 현재 포크의 기능 설명을 대체하지 않습니다.

Lmxxf 코드는 MIT이며 NVIDIA에서 유래한 모델 가중치는 해당 MIT 라이선스의 적용 대상이 아닙니다. 이 포크는 모델을 MIT 또는 GPL로 재라이선스하지 않습니다. 공개 원본 패키지의 존재나 이 출처 기록 자체를 별도 모델 라이선스 허가서로 해석하지 않습니다.

## 설치 구성

새 설치는 동봉 모델을 자동으로 찾습니다. 개인 `nr-assets-path.txt`나 개발 PC의 모델 경로는 ZIP에 넣지 않습니다. Small의 일반 1080p 초기화가 Original 모델도 사용하므로 두 모델을 함께 제공합니다.

NR 실행 경로는 호환 RX 9000/RDNA 4 GPU와 AMD 드라이버를 전제로 합니다. Windows HIP 런타임은 [AMD 공식 배포 지침](https://rocm.docs.amd.com/projects/install-on-windows/en/latest/conceptual/deployment-guidelines.html)에 따라 드라이버가 제공하는 System32의 런타임을 사용합니다. 별도 HIP SDK 설치나 드라이버 DLL의 앱 폴더 복사는 요구하지 않습니다.

개인 로그·캐시·계정·녹화·외부 모델 참조는 포함하지 않습니다. 대응 소스 ZIP은 앱과 실행 코드의 대응 소스를 제공하며 모델 파일은 실행용 ZIP에 포함합니다.
