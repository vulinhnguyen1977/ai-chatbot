<#
.SYNOPSIS
  Copies GGUF weights out of a local Ollama install into runtime/models/.

.DESCRIPTION
  Ollama stores each model layer as a content-addressed blob, and the layer with
  mediaType "application/vnd.ollama.image.model" is a plain GGUF file -- the same
  format llama.cpp loads. So if a model has already been pulled with Ollama, its
  weights can be bundled without downloading anything again.

  This is a packaging helper, not something the end user runs: the shipped zip
  already contains the .gguf files.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File ship\extract_models.ps1
#>
param(
  [hashtable]$Models = @{
    "qwen3:4b"          = "qwen3-4b.gguf"
    "qwen3:1.7b"        = "qwen3-1.7b.gguf"
    "nomic-embed-text"  = "nomic-embed-text.gguf"
  },
  [string]$OllamaStore = $(if ($env:OLLAMA_MODELS) { $env:OLLAMA_MODELS }
                          else { Join-Path $env:USERPROFILE ".ollama\models" }),
  [string]$Destination = (Join-Path (Split-Path $PSScriptRoot -Parent) "runtime\models")
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $OllamaStore)) {
  throw "Ollama model store not found at $OllamaStore"
}
New-Item -ItemType Directory -Force -Path $Destination | Out-Null

foreach ($entry in $Models.GetEnumerator()) {
  $name = $entry.Key
  $outName = $entry.Value

  # "qwen3:4b" -> manifests/registry.ollama.ai/library/qwen3/4b
  $parts = $name.Split(':')
  $repo = $parts[0]
  $tag = if ($parts.Count -gt 1) { $parts[1] } else { "latest" }
  $manifest = Join-Path $OllamaStore "manifests\registry.ollama.ai\library\$repo\$tag"

  if (-not (Test-Path $manifest)) {
    Write-Host "  SKIP $name -- not pulled (run: ollama pull $name)" -ForegroundColor Yellow
    continue
  }

  $json = Get-Content $manifest -Raw | ConvertFrom-Json
  $layer = $json.layers | Where-Object { $_.mediaType -eq "application/vnd.ollama.image.model" } |
           Select-Object -First 1
  if (-not $layer) {
    Write-Host "  SKIP $name -- no model layer in the manifest" -ForegroundColor Yellow
    continue
  }

  $blob = Join-Path $OllamaStore ("blobs\" + $layer.digest.Replace(':', '-'))
  if (-not (Test-Path $blob)) {
    Write-Host "  SKIP $name -- blob missing: $blob" -ForegroundColor Yellow
    continue
  }

  # Verify it really is GGUF before copying gigabytes to the wrong place.
  $stream = [System.IO.File]::OpenRead($blob)
  $magic = New-Object byte[] 4
  [void]$stream.Read($magic, 0, 4)
  $stream.Close()
  if (-join ($magic | ForEach-Object { [char]$_ }) -ne "GGUF") {
    Write-Host "  SKIP $name -- blob is not GGUF" -ForegroundColor Yellow
    continue
  }

  $target = Join-Path $Destination $outName
  $sizeMb = [math]::Round((Get-Item $blob).Length / 1MB, 1)
  if ((Test-Path $target) -and ((Get-Item $target).Length -eq (Get-Item $blob).Length)) {
    Write-Host ("  have $outName ({0:N1} MB)" -f $sizeMb)
    continue
  }
  Write-Host ("  copy $name -> $outName ({0:N1} MB)..." -f $sizeMb)
  Copy-Item $blob -Destination $target -Force
}

Write-Host "`nruntime/models now contains:" -ForegroundColor Cyan
Get-ChildItem $Destination -Filter *.gguf | ForEach-Object {
  Write-Host ("  {0,-28} {1,8:N1} MB" -f $_.Name, ($_.Length / 1MB))
}
