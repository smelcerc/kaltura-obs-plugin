# Build on Windows

Requirements: 64-bit Windows, Visual Studio 2022 with Desktop C++, CMake 3.28+, an OBS 32 SDK
containing Development components, matching Qt 6 development files, Git, and PowerShell 7.
MinGW is not supported.

```powershell
git clone --depth 1 --branch 32.1.2 https://github.com/obsproject/obs-studio.git third_party/obs-studio
cmake -S . -B build-windows -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="C:/obs-sdk;C:/obs-deps;C:/obs-qt" `
  -DKALTURA_LIVE_OBS_SOURCE_PATH="$PWD/third_party/obs-studio"
cmake --build build-windows --config RelWithDebInfo --parallel
ctest --test-dir build-windows -C RelWithDebInfo --output-on-failure
./scripts/package-windows.ps1 -BuildDirectory build-windows -Configuration RelWithDebInfo
```

If the SDK has no CMake package metadata, set `KALTURA_LIVE_OBS_SDK_PATH` plus the two explicit
OBS `.lib` cache variables described in `docs/DEPENDENCIES.md`. The ZIP contains
`obs-plugins/64bit/kaltura-live.dll` and optional `data/obs-plugins/kaltura-live` resources. Extract
it into the OBS installation root or the matching portable root. The packaging script rejects a
non-x64 PE binary with `dumpbin`.
