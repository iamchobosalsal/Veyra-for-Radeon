# SPDX-License-Identifier: GPL-3.0-only
# Serial GPU diagnostics; no capture device, settings, driver or SDK install.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [Parameter(Mandatory = $true)][string]$EvidenceDir
)
$ErrorActionPreference = 'Stop'
$buildPath = (Resolve-Path -LiteralPath $BuildDir).Path
$exe = Join-Path $buildPath 'radeon_pipeline_probe.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Build the radeon_pipeline_probe target first: $exe" }
$evidencePath = [IO.Path]::GetFullPath($EvidenceDir)
New-Item -ItemType Directory -Force -Path $evidencePath | Out-Null
$results = @()
$failed = 0
foreach ($mode in @('sr', 'amf', 'sr-amf', 'fsr3fg', 'fsr4fg')) {
    foreach ($height in @(1080, 1440)) {
        foreach ($fps in @(30, 60)) {
            $name = "$mode-$height-$fps"
            $stdout = Join-Path $evidencePath "$name.log"
            $stderr = Join-Path $evidencePath "$name.err.log"
            $process = Start-Process -FilePath $exe -ArgumentList @($mode, $height, $fps) `
                -WorkingDirectory $buildPath -WindowStyle Hidden -PassThru `
                -RedirectStandardOutput $stdout -RedirectStandardError $stderr
            $timedOut = -not $process.WaitForExit(180000)
            if ($timedOut) { $process.Kill(); $process.WaitForExit() }
            $code = if ($timedOut) { 124 } else { $process.ExitCode }
            $line = @(Get-Content -LiteralPath $stdout | Where-Object { $_.StartsWith('RADEON_PROBE ') })
            if ($line.Count -eq 1) {
                $entry = $line[0].Substring('RADEON_PROBE '.Length) | ConvertFrom-Json
                $entry | Add-Member -NotePropertyName exitCode -NotePropertyValue $code
            } else {
                $entry = [pscustomobject]@{ mode = $mode; height = $height; inputPtsFps = $fps
                    exitCode = $code; error = (Get-Content -LiteralPath $stderr -Raw) }
                if ($code -eq 0) { $failed++ }
            }
            if ($code -ne 0) { $failed++ }
            $results += $entry
            $results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $evidencePath 'pipeline-matrix.json') -Encoding utf8
            Write-Output "$name exit=$code completionMeanMs=$($entry.completionMeanMs) interpolationEvidencePass=$($entry.interpolationEvidencePass)"
            $process.Dispose()
        }
    }
}
Write-Output "Cases=$($results.Count) Failures=$failed; display FPS, pacing, audio and capture latency remain unverified."
if ($failed) { exit 1 }
