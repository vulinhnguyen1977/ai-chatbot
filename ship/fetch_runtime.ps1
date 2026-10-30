<#
.SYNOPSIS
  Downloads the llama.cpp inference engine into runtime/bin.

.DESCRIPTION
  The application ships its own engine so the client installs nothing. This
  fetches the prebuilt Windows binaries and lays them out as the app expects:

    runtime/bin/cpu/      works on any x64 machine
    runtime/bin/vulkan/   GPU acceleration on Intel, AMD and NVIDIA

  Only needed when rebuilding the runtime from scratch or moving to a newer
  llama.cpp release. The shipped zip already contains it.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File ship\fetch_runtime.ps1
  powershell -ExecutionPolicy Bypass -File ship\fetch_runtime.ps1 -Release b10331
#>
param(
  [string]$Release = "b10331",
  [switch]$CpuOnly
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$root = Split-Path $PSScriptRoot -Parent
$runtime = Join-Path $root "runtime"
$staging = Join-Path $runtime "_download"

# Tool executables llama-server does not need. Dropping them saves ~4 MB per
# engine build without touching any shared library.
$unwanted = @(
  "llama-cli.exe", "llama-bench.exe", "llama-perplexity.exe", "llama-quantize.exe",
  "llama-batched-bench.exe", "llama-run.exe", "llama-tokenize.exe", "llama-imatrix.exe",
  "llama-mtmd-cli.exe", "llama-gguf-split.exe",
  "llama-cli-impl.dll", "llama-bench-impl.dll", "llama-perplexity-impl.dll",
  "llama-quantize-impl.dll", "llama-batched-bench-impl.dll", "llama-completion-impl.dll",
  "llama-fit-params-impl.dll"
)

$targets = @(@{ Name = "cpu"; Asset = "llama-$Release-bin-win-cpu-x64.zip" })
if (-not $CpuOnly) {
  $targets += @{ Name = "vulkan"; Asset = "llama-$Release-bin-win-vulkan-x64.zip" }
}

New-Item -ItemType Directory -Force -Path $runtime, (Join-Path $runtime "models") | Out-Null

foreach ($t in $targets) {
  $url = "https://github.com/ggml-org/llama.cpp/releases/download/$Release/$($t.Asset)"
  $zip = Join-Path $runtime "$($t.Name).zip"
  $extract = Join-Path $staging $t.Name
  $dest = Join-Path $runtime "bin\$($t.Name)"

  Write-Host "Downloading $($t.Asset)..." -ForegroundColor Cyan
  Invoke-WebRequest $url -OutFile $zip -UseBasicParsing -TimeoutSec 900

  Expand-Archive $zip -DestinationPath $extract -Force
  New-Item -ItemType Directory -Force -Path $dest | Out-Null

  # The archives put binaries in a nested folder that varies between releases,
  # so collect by extension rather than by path.
  Get-ChildItem $extract -Recurse -File |
    Where-Object { $_.Extension -in ".exe", ".dll" } |
    Copy-Item -Destination $dest -Force

  foreach ($name in $unwanted) {
    $p = Join-Path $dest $name
    if (Test-Path $p) { [System.IO.File]::Delete((Resolve-Path $p).Path) }
  }

  if (-not (Test-Path (Join-Path $dest "llama-server.exe"))) {
    throw "llama-server.exe not found in $($t.Asset) -- the release layout may have changed."
  }

  $size = (Get-ChildItem $dest -File | Measure-Object Length -Sum).Sum / 1MB
  Write-Host ("  runtime\bin\{0}: {1} files, {2:N1} MB" -f $t.Name,
              (Get-ChildItem $dest -File).Count, $size) -ForegroundColor Green

  [System.IO.File]::Delete((Resolve-Path $zip).Path)
}

if (Test-Path $staging) { [System.IO.Directory]::Delete((Resolve-Path $staging).Path, $true) }

Write-Host "`nEngine ready. Next: put .gguf weights in runtime\models" -ForegroundColor Cyan
Write-Host "  (ship\extract_models.ps1 copies them out of a local Ollama install)"
