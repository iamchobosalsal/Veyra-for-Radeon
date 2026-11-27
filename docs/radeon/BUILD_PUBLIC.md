# Building the Radeon community preview

This is the modified source for `v1.0.0-radeon-preview.1`, based on upstream Veyra commit `671f9e963db63cbd7f912caa35c4caa3e0e60f76`. The original authors retain their copyrights. This snapshot records changes through 2026-11-27. It is not an upstream Veyra release.

`SOURCE_SNAPSHOT.json` records source-file hashes and the accepted runtime identities. The snapshot excludes the uninstalled source-anchor experiment. Source receipt/hash matching is not a claim of byte-for-byte reproducible MSVC output.

## Dependencies

Use Windows x64, a Windows SDK with D3D12, MSVC 14.44 (Visual Studio 2022 tools), CMake/Ninja, Qt 6.8.3 MSVC2022 x64 and DXC 1.8.2505.1. Build externally obtained SDKs/dependencies outside the source checkout. Do not commit SDK binaries, headers with incompatible redistribution terms, private models, credentials or user profiles.

The accepted application uses the patched FFmpeg 9.0.1 build with H.264 slices256, dav1d and zlib support. Do not silently substitute an older upstream FFmpeg binary. The associated source archive, patch/build information and component notices accompany a binary release. Qt is dynamically linked; preserve the supplied LGPL/GPL notices and the ability to replace the libraries. The third-party audit/source inventory identifies dependency versions and sources.

The FFmpeg prefix must also provide the matching `libavfilter/avfilter.h` from the separate deinterlace module and the `share/ffmpeg/FindFFMPEG.cmake` integration from the recorded dependency build. Keep the matching deinterlace runtime when packaging. A vanilla FFmpeg source checkout is not an installed development prefix.

The current graph still has a build dependency on the NVIDIA Optical Flow SDK headers (`NvOFInterface/nvOpticalFlowCommon.h`), even though NVIDIA optical flow is disabled in this Radeon configuration. Supply the external 5.0.7 SDK directory. The sinks also require `nv-codec-headers` n13.0.19.0 (`include/ffnvcodec/nvEncodeAPI.h`). The portable update manifest code requires OpenSSL Crypto; the accepted build used OpenSSL 3.6.1. Supply matching headers and an import library and retain the corresponding runtime when packaging.

Some optional interfaces still require separately licensed SDK headers to compile. Their presence in the build inputs does not grant permission to redistribute their binaries or models. The Core runtime deliberately excludes FidelityFX SDK 2.3.0 signed DLLs and NR model weights. Proprietary third-party source is not part of this repository.

## Application configuration

Run from a VS x64 developer environment. Set the dependency paths below to your own acquired dependencies. Refer to the root `CMakeLists.txt` for the supported cache parameters and to `SOURCE_SNAPSHOT.json` for the accepted build settings.

```powershell
cmake -S . -B build-radeon -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH=C:/deps/Qt/6.8.3/msvc2022_64 `
  -DVEYRA_FFMPEG_ROOT=C:/deps/ffmpeg-9.0.1-patched `
  -DVEYRA_DLSS_SDK_ROOT=C:/deps/dlss `
  -DVEYRA_NVOF_SDK_ROOT=C:/deps/Optical_Flow_SDK_5.0.7 `
  -DVEYRA_NVENC_HEADERS_ROOT=C:/deps/nv-codec-headers-n13.0.19.0 `
  -DOPENSSL_INCLUDE_DIR=C:/deps/openssl-3.6.1/include `
  -DOPENSSL_CRYPTO_LIBRARY=C:/deps/openssl-3.6.1/lib/libcrypto.lib `
  -DVEYRA_FIDELITYFX_ROOT=C:/deps/FidelityFX-1.1.4/sdk `
  -DVEYRA_FIDELITYFX2_ROOT=C:/deps/FidelityFX-SDK-2.3.0/Kits/FidelityFX `
  -DVEYRA_AMF_SDK_ROOT=C:/deps/AMF-1.5.3/amf/public `
  -DVEYRA_DXC_EXECUTABLE=C:/deps/dxc/bin/x64/dxc.exe `
  -DVEYRA_RADEON_FORK=ON `
  -DVEYRA_DISPLAY_VERSION=1.0.0-radeon-preview `
  -DVEYRA_UPDATE_INSTALL_ENABLED=OFF `
  -DVEYRA_UPDATE_REPOSITORY= `
  -DVEYRA_UPDATE_PUBLIC_KEY= `
  -DVEYRA_BUILD_QML_UI=ON `
  -DVEYRA_ENABLE_D3D12_DEBUG=OFF `
  -DVEYRA_ENABLE_DEINTERLACE=ON `
  -DVEYRA_ENABLE_DLSS_FG=OFF `
  -DVEYRA_ENABLE_DLSS_SR=OFF `
  -DVEYRA_ENABLE_EXPERIMENTAL_DLSSNR=ON `
  -DVEYRA_ENABLE_RIFE_DML=OFF `
  -DVEYRA_ENABLE_NVOF=OFF `
  -DVEYRA_ENABLE_REMOTEPLAY=OFF `
  -DVEYRA_ENABLE_MOONLIGHT=OFF `
  -DVEYRA_ENABLE_XBOX=OFF `
  -DVEYRA_ENABLE_LIBDOVI=OFF `
  -DVEYRA_BUILD_VIRTUALCAM_INSTALLER=OFF
cmake --build build-radeon --target veyra_qml_ui veyra_portable_launcher veyra_update_worker --parallel 2
```

The application target builds its dependent launcher, update worker and shaders. Package its matching `shaders` directory and QML tree together with the executable; do not combine a newly built executable with shaders from another experiment. The paths above are examples; adapt dependency import-library filenames to the development prefixes you built.

The accepted RIFE provider is a separate Veyra-built shared library, not the direct in-process `VEYRA_ENABLE_RIFE_DML` option. Its ABI compatibility filename is `runtime/experimental/anr/nvngx_dlssg.dll`; it is not NVIDIA's DLL. Build with `scripts/radeon/build_rife_dml.ps1`, supplying `-Compiler`, `-Dependencies`, `-Output` and `-NgxHeaders`. That helper compiles `RifeParameterAbi.cpp`, `RifeVideoFgProvider.cpp` and the DirectML RIFE backend. The required DirectML 1.15.4 headers and DirectMLX headers are external build inputs. `third_party/rife/UPSTREAM.json` pins the RIFE model provenance and hashes.

The NR host library's corresponding source is a separate dependency artifact. NR weights are not included and must not be inferred from the host library's MIT license.

## Packaging and checks

See [PACKAGING.md](PACKAGING.md) for audited runtime inventory, source archive matching and manual prerelease packaging. Never package a personal installation recursively. Core omission of FSR must be explicit in the manifest; a partial DLL set is not a valid package.

```powershell
python tests/radeon/test_package_release.py -q
python tests/radeon/test_prepare_local_preview.py -q
```

These are packaging tests, not claims of GPU/video quality validation. Run the relevant native tests in a private data directory for changes to rendering, capture or the updater. Do not publish test screenshots, user settings or local authentication data.
