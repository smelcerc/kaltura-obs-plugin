# Dependencies

## Policy

Release builds use pinned upstream source or official OBS build dependencies. Do not commit local
Homebrew libraries, OBS application binaries, Qt frameworks, Whisper models, or other precompiled
machine-specific files. All architectures in one process must match.

| Dependency | Version/baseline | macOS x86_64 / arm64 | Windows x86_64 | Linux x86_64 | Discovery |
|---|---|---|---|---|---|
| OBS Studio SDK | 32.0 minimum; CI 32.1.2 | matching OBS app/official SDK | official OBS SDK build | `libobs-dev` + frontend library | CMake config first, configurable SDK fallback |
| Qt | Qt 6, ABI-compatible with OBS | OBS-matched Qt package | official OBS Qt dependency | distribution `qt6-base-dev` matching OBS | `find_package(Qt6)` imported targets |
| whisper.cpp | v1.9.1 | source build per slice | source build | source build | pinned `FetchContent` |
| ggml | supplied by whisper.cpp | Accelerate/CPU, native tuning off | CPU, native tuning off | CPU/BLAS where found, native tuning off | whisper.cpp target |
| libcaption | selected OBS source tag | source | source | source | `KALTURA_LIVE_OBS_SOURCE_PATH/deps/libcaption` |
| Secure storage | system backend | Keychain | Credential Manager (`Advapi32`) | libsecret/Secret Service | framework/system library/pkg-config |

Whisper models are runtime data, not linked libraries. `download-whisper-models.sh` downloads the
known model names and verifies pinned SHA-256 hashes. Packages can omit models for CI smoke builds.

## Updating

Update one dependency at a time. Rebuild all five native CI targets, recreate Universal 2 from the
two macOS slices, inspect dynamic dependencies, and execute the runtime matrix. An OBS or Qt update
requires verifying Qt major/minor compatibility with the exact OBS release. A whisper.cpp update
also requires checking its CMake CPU dispatch and Universal 2 behavior.

Environment/cache inputs are `CMAKE_PREFIX_PATH`, `KALTURA_LIVE_OBS_SOURCE_PATH`,
`KALTURA_LIVE_OBS_SDK_PATH`, `KALTURA_LIVE_LIBOBS_LIBRARY`,
`KALTURA_LIVE_OBS_FRONTEND_LIBRARY`, `KALTURA_LIVE_OBS_APP_PATH`, `QT_PREFIX`, and
`KALTURA_LIVE_MODEL_SOURCE_DIR`.
