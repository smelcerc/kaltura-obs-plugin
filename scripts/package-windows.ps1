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
$Stream = [IO.File]::OpenRead($Binary)
$Reader = [IO.BinaryReader]::new($Stream)
try {
  $Stream.Position = 0x3c
  $PeOffset = $Reader.ReadInt32()
  $Stream.Position = $PeOffset
  $PeSignature = $Reader.ReadUInt32()
  $Machine = $Reader.ReadUInt16()
} finally {
  $Reader.Dispose()
  $Stream.Dispose()
}
if ($PeSignature -ne 0x00004550 -or $Machine -ne 0x8664) {
  $Message = 'Architecture validation failed: expected an x64 PE image, signature={0:X8}, machine={1:X4}' `
    -f $PeSignature, $Machine
  throw $Message
}

New-Item -ItemType Directory -Force -Path $DistDirectory | Out-Null
$Archive = Join-Path $DistDirectory "kaltura-live-$Version-Windows-x86_64.zip"
if (Test-Path $Archive) { Remove-Item -Force $Archive }
Compress-Archive -Path (Join-Path $StageDirectory '*') -DestinationPath $Archive
Write-Host "Created $Archive"
