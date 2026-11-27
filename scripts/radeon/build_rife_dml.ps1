param(
    [string]$Compiler = 'C:\capture\deps\llvm-mingw-20261117-ucrt-x86_64\bin\clang++.exe',
    [string]$Dependencies = 'C:\capture\evidence\video-fg-20261124',
    [string]$Output = 'C:\capture\evidence\video-fg-20261124',
    [string]$NgxHeaders = 'C:\capture\deps\nvidia-dlss-sdk\include'
)
$ErrorActionPreference = 'Stop'
$sourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Output = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Path $Output -Force | Out-Null
$originalTemp = $env:TEMP
$originalTmp = $env:TMP
try {
    $env:TEMP = $Output
    $env:TMP = $Output
    $common = @('-std=c++20', '-O2', '-DNOMINMAX', '-DUNICODE', '-D_UNICODE', '-DDML_TARGET_VERSION=0x5000',
        '-I', "$sourceRoot/include", '-I', $Dependencies, '-I', "$Dependencies/directml-1.15.4/include",
        "$sourceRoot/src/pipeline/RifeDmlBackend.cpp", "$sourceRoot/src/base/Log.cpp", "$sourceRoot/src/base/RuntimePaths.cpp",
        '-static', '-ld3d12', '-ld3dcompiler', '-ldxgi', '-ldxguid', '-luser32')
    & $Compiler @common "$sourceRoot/tests/radeon/RifeDmlProbe.cpp" '-o' "$Output/rife_dml_probe.exe"
    if ($LASTEXITCODE -ne 0) { throw "RIFE probe build failed: $LASTEXITCODE" }
    & $Compiler '--target=x86_64-pc-windows-msvc' '-std=c++20' '-O2' '-fno-exceptions' '-fno-rtti' '-I' $NgxHeaders '-c' "$sourceRoot/src/ngx/RifeParameterAbi.cpp" '-o' "$Output/rife_parameter_abi.obj"
    if ($LASTEXITCODE -ne 0) { throw "RIFE parameter ABI build failed: $LASTEXITCODE" }
    & $Compiler @common '-I' $NgxHeaders '-shared' "$sourceRoot/src/ngx/RifeVideoFgProvider.cpp" "$sourceRoot/src/ngx/RifeVideoFgProvider.def" "$Output/rife_parameter_abi.obj" '-o' "$Output/veyra_rife_fg.dll"
    if ($LASTEXITCODE -ne 0) { throw "RIFE provider build failed: $LASTEXITCODE" }
} finally {
    $env:TEMP = $originalTemp
    $env:TMP = $originalTmp
}
