# Build on Linux

Ubuntu 24.04 is the release baseline. GCC and Clang are supported.

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build git pkg-config \
  obs-studio libobs-dev qt6-base-dev libsimde-dev libsecret-1-dev
git clone --depth 1 --branch 32.1.2 https://github.com/obsproject/obs-studio.git third_party/obs-studio
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-linux --parallel
ctest --test-dir build-linux --output-on-failure
./scripts/validate-architecture.sh linux build-linux/libkaltura-live.so x86_64
cmake --build build-linux --target package
```

The DEB installs system-wide below `/usr/lib/*/obs-plugins` and `/usr/share/obs/obs-plugins`.
For a user-local manual install, copy the module to
`~/.config/obs-studio/plugins/kaltura-live/bin/64bit/` and data to
`~/.config/obs-studio/plugins/kaltura-live/data/`. Distribution multiarch library directories can
vary; inspect `cmake --install ... --prefix <stage>` before system-wide manual copying.

Secret persistence requires libsecret at build time and an available Secret Service at runtime.
Flatpak deployment is not part of the initial artifact: its sandbox must expose the plugin, models,
network, audio, and Secret Service permissions. A future Flatpak extension should use the same
installed module/data split.
