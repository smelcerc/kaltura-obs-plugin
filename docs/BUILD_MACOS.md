# Build on macOS

Install CMake, Ninja, and the Qt 6 version used by your OBS installation. Clone the matching OBS
source tag for headers and libcaption. `QT_PREFIX` can point to an aqt installation; Homebrew users
can obtain the prefix from the installed `qt`, `qtbase`, or `qt@6` formula.

```bash
git clone --depth 1 --branch 32.1.2 https://github.com/obsproject/obs-studio.git third_party/obs-studio
cmake -S . -B build-macos -G Ninja \
  -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase)" \
  -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build-macos --parallel
ctest --test-dir build-macos --output-on-failure
```

Use `x86_64` on Intel. The OBS app, Qt, and every linked dependency must contain the requested
slice. `scripts/deploy-local-macos.sh` builds and installs a local development bundle.

## Universal 2

whisper.cpp v1.9.1 configures architecture-specific ggml CPU flags once per build tree, and the
downloadable OBS apps contain a single native slice. Build once against Intel OBS and once against
Apple OBS, package each, then merge:

```bash
./scripts/package-macos-universal.sh path/to/x86_64/kaltura-live.plugin \
  path/to/arm64/kaltura-live.plugin dist
lipo -archs dist-unpacked/kaltura-live.plugin/Contents/MacOS/kaltura-live
```

CI performs this merge and requires `x86_64 arm64`. Do not use Rosetta as the arm64 build path.
Local packages are ad-hoc signed. For distribution, set `MACOS_APPLICATION_SIGNING_IDENTITY` and
`MACOS_INSTALLER_SIGNING_IDENTITY`; set `MACOS_NOTARIZE=true` and an `APPLE_NOTARY_PROFILE` created
with `notarytool` to notarize.
