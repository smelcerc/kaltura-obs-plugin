# Developer Documentation

## Architecture

- `Plugin` owns the OBS lifecycle, dock/settings UI, persistence hooks, API client, streaming
  manager, and caption pipeline.
- `KalturaApiClient` is UI-independent and uses an injected asynchronous `HttpTransport`.
- `StreamingManager` configures the standard OBS output and an optional auxiliary RTMP output.
- `CaptionProvider` abstracts speech recognition. `WhisperProvider` captures OBS program mix 1,
  performs background inference, and returns final text.
- `Cea608CaptionInserter` normalizes and splits text into one- or two-line, 32-column screens and
  hands them to OBS native frame-timed caption insertion.
- `SettingsManager` validates and persists project settings through OBS save callbacks.

Networking and inference never block the Qt UI thread. UI updates cross to the main thread using
queued Qt invocations.

## Prerequisites

- CMake 3.28+
- Ninja
- C++20 compiler
- Qt 6 Core, Gui, Network, and Widgets development files
- OBS Studio 32.2+ development packages, or OBS.app plus matching OBS source headers on macOS
- Git and curl for dependency/model retrieval
- The `file` and `dpkg-dev` packages for Debian release packaging

`whisper.cpp` is pinned by CMake. OBS source is intentionally ignored and must be cloned locally:

```bash
git clone --depth 1 --branch 32.2.1 https://github.com/obsproject/obs-studio.git third_party/obs-studio
```

## macOS

Install dependencies with Homebrew and deploy locally:

```bash
brew install cmake ninja qtbase simde
./scripts/deploy-local-macos.sh
```

The Qt development version must exactly match OBS's bundled Qt runtime. The deploy script rejects
external Qt runtime linkage, patches relocatable rpaths, verifies architecture, and ad-hoc signs
the local bundle.

## Linux

Install OBS/libobs and Qt 6 development packages, then:

```bash
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-linux --parallel
ctest --test-dir build-linux --output-on-failure
```

Linux resolves models through `obs_get_module_data_path()` under `models/`.

## Tests

- API client: mocked async transport, retries, validation, and typed parsing
- Caption provider: dictionary and transcript behavior
- CEA-608: normalization, wrapping, continuation screens, timing, and observable delivery health

Run `./scripts/audit-release.sh` before committing. Never add real KS values, stream keys, HAR
files, OBS logs, models, signing files, or generated packages.

## Adding a caption provider

Implement `CaptionProvider`, keep recognition off the UI thread, consume the supplied 16 kHz mono
program audio, and return final text through the callback. Provider code must not contain UI or
stream-output logic.

## API client changes

Keep operations asynchronous, bounded by configurable timeouts, safely parse every JSON type, and
return typed models/errors. Add mock fixtures using `example.test` domains and synthetic values.
