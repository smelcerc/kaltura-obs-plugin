param(
  [string] $BuildDirectory = "$PSScriptRoot/../build-windows",
  [string] $DistDirectory = "$PSScriptRoot/../dist",
  [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
  [string] $Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$ProjectDirectory = Resolve-Path "$PSScriptRoot/.."
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$DistDirectory = [IO.Path]::GetFullPath($DistDirectory)
$StageDirectory = Join-Path $BuildDirectory 'package-stage'
$Version = (Get-Content (Join-Path $ProjectDirectory 'VERSION') -Raw).Trim()

cmake --build $BuildDirectory --config $Configuration --parallel
ctest --test-dir $BuildDirectory -C $Configuration --output-on-failure
if (Test-Path $StageDirectory) { Remove-Item -Recurse -Force $StageDirectory }
cmake --install $BuildDirectory --config $Configuration --prefix $StageDirectory

$Binary = Join-Path $StageDirectory 'obs-plugins/64bit/kaltura-live.dll'
if (!(Test-Path $Binary)) { throw "Packaged plugin binary was not found: $Binary" }
$Headers = & dumpbin /headers $Binary | Out-String
if ($LASTEXITCODE -ne 0 -or $Headers -notmatch 'machine \(x64\)') {
  throw 'Architecture validation failed: plugin is not Windows x64'
}

New-Item -ItemType Directory -Force -Path $DistDirectory | Out-Null
$Archive = Join-Path $DistDirectory "kaltura-live-$Version-Windows-x86_64.zip"
if (Test-Path $Archive) { Remove-Item -Force $Archive }
Compress-Archive -Path (Join-Path $StageDirectory '*') -DestinationPath $Archive
Write-Host "Created $Archive"
