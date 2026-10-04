# Downloads the FluidSynth WebAssembly library (js-synthesizer) into ./lib so the
# SoundFont player works fully offline afterwards. Run once, with internet access.
#
#   powershell -ExecutionPolicy Bypass -File fetch-libs.ps1

$ErrorActionPreference = 'Stop'
$ver = '1.13.0'                       # js-synthesizer version (bundles libfluidsynth 2.4.6)
$base = "https://unpkg.com/js-synthesizer@$ver"
$libDir = Join-Path $PSScriptRoot 'lib'
New-Item -ItemType Directory -Force $libDir | Out-Null

$files = @{
  "$base/externals/libfluidsynth-2.4.6.js" = 'libfluidsynth-2.4.6.js'
  "$base/dist/js-synthesizer.js"           = 'js-synthesizer.js'
}

foreach ($url in $files.Keys) {
  $out = Join-Path $libDir $files[$url]
  Write-Host "Downloading $url"
  Invoke-WebRequest -Uri $url -OutFile $out
}

# Starter SoundFont: GeneralUser GS (S. Christian Collins) — piano + full General
# MIDI + a drum kit, ~31 MB. Skipped if already present.
$sfDir = Join-Path $PSScriptRoot 'soundfonts'
New-Item -ItemType Directory -Force $sfDir | Out-Null
$sf = Join-Path $sfDir 'GeneralUser-GS.sf2'
if (Test-Path $sf) {
  Write-Host "Starter SoundFont already present: $sf"
} else {
  $sfUrl = 'https://github.com/mrbumpy409/GeneralUser-GS/raw/main/GeneralUser-GS.sf2'
  Write-Host "Downloading starter SoundFont (~31 MB)..."
  # curl.exe handles the GitHub redirect and shows progress; fall back to IWR.
  if (Get-Command curl.exe -ErrorAction SilentlyContinue) {
    & curl.exe -fL $sfUrl -o $sf
  } else {
    Invoke-WebRequest -Uri $sfUrl -OutFile $sf
  }
}

Write-Host "`nDone." -ForegroundColor Green
Write-Host "  Libraries:  $libDir"
Write-Host "  SoundFont:  $sf"
Write-Host "Run a local server in soundplayer/ (python -m http.server 8000) and open http://localhost:8000"
