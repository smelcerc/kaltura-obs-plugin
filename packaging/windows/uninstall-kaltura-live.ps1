$ErrorActionPreference = 'Stop'

if (Get-Process obs64 -ErrorAction SilentlyContinue) {
  throw 'Quit OBS Studio before uninstalling Kaltura Live.'
}

$Targets = @(
  (Join-Path $PSScriptRoot 'obs-plugins/64bit/kaltura-live.dll'),
  (Join-Path $PSScriptRoot 'data/obs-plugins/kaltura-live'),
  (Join-Path $env:APPDATA 'obs-studio/plugins/kaltura-live'),
  (Join-Path $env:APPDATA 'obs-studio/plugins/kaltura-live.plugin')
)

foreach ($Target in $Targets) {
  if (Test-Path -LiteralPath $Target) {
    Remove-Item -LiteralPath $Target -Recurse -Force
    Write-Host "Removed $Target"
  }
}

Write-Host 'Kaltura Live was uninstalled. You can now delete this script.'

