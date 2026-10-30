<#
.SYNOPSIS
  Builds a shippable zip of the tutor.

.DESCRIPTION
  Produces ship/mstutor-<date>.zip: a self-contained desktop application. The
  client unzips it, double-clicks mstutor.exe, and it works -- no Ollama, no
  Python, no runtime to install.

  The inference engine (llama.cpp) and the model weights live in runtime/ and
  are included by default, which is what makes the package several gigabytes.
  Use -NoModels for a small update package aimed at a machine that already has
  runtime/models populated.

  Licensing: bundling weights means redistributing someone else's model. Qwen3
  and nomic-embed-text are Apache-2.0 and may be redistributed with
  attribution. If you swap in a Llama or Gemma model, re-read its licence
  first. See THIRD_PARTY_NOTICES.md.

.PARAMETER NoModels
  Exclude runtime/models/*.gguf. The engine is still included.

.PARAMETER SkipModels
  Exclude individual weight files by name, e.g. -SkipModels qwen3-4b.gguf.
  GitHub release assets are capped at 2 GiB, so the public release ships the
  Fast model in the zip and the Smart one separately (see get-smart-model.bat).

.PARAMETER NoSource
  Exclude src/ and third_party/ for a binary-only delivery.

.PARAMETER CpuOnly
  Exclude the Vulkan engine build, saving about 90 MB. The CPU build works
  everywhere; the Vulkan one is a large speed-up on machines that have a GPU.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File ship\package.ps1
  powershell -ExecutionPolicy Bypass -File ship\package.ps1 -NoModels -NoSource
  powershell -ExecutionPolicy Bypass -File ship\package.ps1 -SkipModels qwen3-4b.gguf
#>
param(
  [switch]$NoModels,
  [string[]]$SkipModels = @(),
  [switch]$NoSource,
  [switch]$CpuOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$staging = Join-Path $PSScriptRoot "staging"
$stamp = Get-Date -Format "yyyy-MM-dd"
$zipPath = Join-Path $PSScriptRoot "mstutor-$stamp.zip"

Write-Host "Packaging from $root" -ForegroundColor Cyan

# --- 1. the binary must exist and must pass its own tests ------------------
$exe = Join-Path $root "mstutor.exe"
if (-not (Test-Path $exe)) {
  throw "mstutor.exe not found. Run build.bat first."
}

Write-Host "`nRunning self-tests before packaging..." -ForegroundColor Cyan
& $exe selftest | Select-Object -Last 3
if ($LASTEXITCODE -ne 0) {
  throw "Self-tests failed. Refusing to package a broken build."
}

# --- 2. stage the tree -----------------------------------------------------
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
New-Item -ItemType Directory -Path $staging -Force | Out-Null

$files = @("mstutor.exe", "README.md", "THIRD_PARTY_NOTICES.md", "LICENSE",
           "start.bat", "build.bat", "CMakeLists.txt", "Makefile",
           "get-smart-model.bat")
foreach ($f in $files) {
  $p = Join-Path $root $f
  if (Test-Path $p) { Copy-Item $p -Destination $staging }
}

# licenses/ holds the llama.cpp and Apache-2.0 texts that must travel with the
# bundled engine and weights, so it ships even in a binary-only package.
$dirs = @("config", "training", "licenses")
# res/ holds the icon and version resource, so it is part of the source build.
if (-not $NoSource) { $dirs += @("src", "third_party", "res") }

foreach ($d in $dirs) {
  $src = Join-Path $root $d
  if (-not (Test-Path $src)) { continue }
  Write-Host "  + $d"
  Copy-Item $src -Destination (Join-Path $staging $d) -Recurse
}

# --- the inference engine and weights --------------------------------------
$runtimeSrc = Join-Path $root "runtime"
if (-not (Test-Path (Join-Path $runtimeSrc "bin\cpu\llama-server.exe"))) {
  throw "runtime\bin\cpu\llama-server.exe is missing. See ship\fetch_runtime.ps1."
}

$runtimeDst = Join-Path $staging "runtime"
New-Item -ItemType Directory -Force -Path (Join-Path $runtimeDst "models") | Out-Null

Write-Host "  + runtime\bin\cpu"
Copy-Item (Join-Path $runtimeSrc "bin\cpu") -Destination (Join-Path $runtimeDst "bin\cpu") -Recurse

if (-not $CpuOnly -and (Test-Path (Join-Path $runtimeSrc "bin\vulkan"))) {
  Write-Host "  + runtime\bin\vulkan (GPU acceleration)"
  Copy-Item (Join-Path $runtimeSrc "bin\vulkan") -Destination (Join-Path $runtimeDst "bin\vulkan") -Recurse
}

if ($NoModels) {
  Write-Host "  - runtime\models (excluded by -NoModels)" -ForegroundColor Yellow
  Set-Content -Path (Join-Path $runtimeDst "models\README.txt") -Encoding utf8 -Value @"
This package was built with -NoModels, so it contains no weights.

Put one or more .gguf files here. The application picks the largest file whose
name does not contain "embed" as the tutor model, and uses a file named
nomic-embed-text.gguf (if present) for textbook search.

Weights can be produced from an Ollama install with ship\extract_models.ps1, or
downloaded from Hugging Face in GGUF format.
"@
} else {
  $models = Get-ChildItem (Join-Path $runtimeSrc "models") -Filter *.gguf -ErrorAction SilentlyContinue |
            Where-Object { $SkipModels -notcontains $_.Name }
  if (-not $models) {
    throw "No .gguf files in runtime\models. Run ship\extract_models.ps1, or pass -NoModels."
  }
  foreach ($s in $SkipModels) {
    Write-Host "  - runtime\models\$s (excluded by -SkipModels)" -ForegroundColor Yellow
  }
  foreach ($m in $models) {
    Write-Host ("  + runtime\models\{0} ({1:N0} MB)" -f $m.Name, ($m.Length / 1MB))
    Copy-Item $m.FullName -Destination (Join-Path $runtimeDst "models")
  }
}

# --- 3. strip anything that must not be shipped ----------------------------
# The database holds real user accounts, password hashes and chat history, and
# the outbox holds live verification codes. Neither belongs in a delivery.
$strip = @(
  "training\data\extracted",
  "training\data\corpus.jsonl",
  "training\data\dataset",
  "training\llama.cpp",
  "training\__pycache__",
  "training\Modelfile"
)
foreach ($s in $strip) {
  $p = Join-Path $staging $s
  if (Test-Path $p) { Remove-Item $p -Recurse -Force }
}

# Keep the sample so a fresh install can exercise the pipeline, drop the rest:
# the client's own textbooks are their licensed material, not ours to forward.
$books = Join-Path $staging "training\data\textbooks"
if (Test-Path $books) {
  Get-ChildItem $books -File | Where-Object { $_.Name -ne "sample_physics_notes.txt" } |
    Remove-Item -Force
}

# An empty data/ so the app has somewhere to create its database on first run.
New-Item -ItemType Directory -Path (Join-Path $staging "data") -Force | Out-Null
Set-Content -Path (Join-Path $staging "data\README.txt") -Encoding utf8 -Value @"
This folder holds runtime state and is created empty on purpose.

  mstutor.db     accounts, sessions, chats, messages and the textbook index
  outbox/        verification emails, when mail.transport = file
  remember.bin   the stored "keep me signed in" token, if used

Nothing here is shipped: the database would contain real accounts and password
hashes, the outbox would contain live verification codes, and remember.bin is a
live session credential.

remember.bin is encrypted with the Windows Data Protection API and is tied to
the Windows account that created it, so copying it elsewhere achieves nothing.
Deleting it simply means signing in again.

Back up mstutor.db to preserve user accounts and chat history across upgrades.
"@

# --- 5. zip ----------------------------------------------------------------
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Write-Host "`nCompressing..." -ForegroundColor Cyan

# Entries are added by hand rather than with Compress-Archive or
# ZipFile.CreateFromDirectory. Both write backslash path separators when run on
# .NET Framework (which is what Windows PowerShell 5.1 uses). The ZIP spec
# requires forward slashes, and backslashes break extraction on macOS and Linux
# tooling -- which matters for something whose whole purpose is to be shipped.
# ZipFile/ZipFileExtensions live in System.IO.Compression.FileSystem, but
# ZipArchive and ZipArchiveMode live in System.IO.Compression. Both are needed.
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::Open(
  $zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
  $prefix = (Resolve-Path $staging).Path.TrimEnd('\') + '\'
  foreach ($file in Get-ChildItem $staging -Recurse -File) {
    $relative = $file.FullName.Substring($prefix.Length).Replace('\', '/')
    [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
      $archive, $file.FullName, $relative,
      [System.IO.Compression.CompressionLevel]::Optimal)
  }
} finally {
  $archive.Dispose()
}

Remove-Item $staging -Recurse -Force

$mb = (Get-Item $zipPath).Length / 1MB
Write-Host ("`nWrote {0} ({1:N2} GB)" -f $zipPath, ($mb / 1024)) -ForegroundColor Green
Write-Host "`nOn the target machine:"
Write-Host "  1. unzip"
Write-Host "  2. double-click mstutor.exe"
if ($NoModels) {
  Write-Host "`n  NOTE: built with -NoModels. Put a .gguf into runtime\models first," -ForegroundColor Yellow
  Write-Host "        or the app will start with no tutor model." -ForegroundColor Yellow
} else {
  Write-Host "`nNothing else needs installing: the engine and the weights are inside."
}
