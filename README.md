# Kaltura Live for OBS Studio

Kaltura Live is an OBS Studio 32.0+ plugin for selecting Kaltura live entries, configuring Primary
and Backup RTMP outputs, monitoring output health, and generating private local CEA-608 captions
with Whisper.

## Features

- Masked Kaltura Session validation against the US Kaltura API endpoint
- Paged, searchable live-entry browser with thumbnails
- Primary, Backup, or independently controlled simultaneous outputs
- Automatic OBS RTMP URL and stream-key configuration with confirmation and rollback
- Independent bitrate, dropped-frame, reconnect, and latency health displays
- Local Whisper Tiny/Base transcription; program audio never leaves the computer
- Broadcast-safe CEA-608 insertion, program delay, placement, alignment, and health monitoring
- Custom vocabulary/correction dictionary with CSV import
- OBS-native settings persistence, theme support, and debug diagnostics

## Install

| Platform | Architecture | Status |
|---|---|---|
| macOS | Apple Silicon arm64 | Supported (native) |
| macOS | Intel x86_64 | Supported (native) |
| macOS | Universal 2 | Supported by merging validated native slices |
| Windows | x86_64 | Supported with Visual Studio 2022/MSVC |
| Linux | x86_64 | Supported; Ubuntu 24.04 baseline |

Download the package for your operating system from GitHub Releases:

- **macOS:** choose the arm64, x86_64, or Universal 2 artifact. The installer copies the plugin into
  the current user's OBS plugin directory. Portable `.tar.gz` artifacts can be copied manually.
- **Windows:** extract the x86_64 ZIP into the OBS installation or portable root so its
  `obs-plugins` and `data` directories merge with OBS.
- **Ubuntu/Debian:** `sudo apt install ./kaltura-live_VERSION_amd64.deb`.
- **Other Linux:** extract the Linux `.tar.gz` under `/` using your distribution's packaging
  conventions.

Restart OBS, then open **Tools → Kaltura Live Settings…**. See the
[User Guide](docs/USER_GUIDE.md) for setup and operation.

## Build and contribute

See the [macOS](docs/BUILD_MACOS.md), [Windows](docs/BUILD_WINDOWS.md), and
[Linux](docs/BUILD_LINUX.md) build guides, the [dependency policy](docs/DEPENDENCIES.md), the
[cross-platform audit](docs/CROSS_PLATFORM_AUDIT.md), [Contributing](CONTRIBUTING.md), and the
[release process](docs/RELEASE_PROCESS.md).

```bash
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/path/to/qt6
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For local macOS development, `./scripts/deploy-local-macos.sh` configures, builds, validates Qt/OBS
runtime compatibility, and deploys to the current user's OBS plugin directory.

## Security and privacy

- KS values are masked, never intentionally logged, and persisted only in macOS Keychain, Windows
  Credential Manager, or Linux Secret Service. Legacy plaintext settings are migrated.
- Transcription runs locally through `whisper.cpp`.
- Release models are downloaded over HTTPS and verified against pinned SHA-256 checksums.
- `./scripts/audit-release.sh` rejects common secret files, KS values, stream-token URLs, private
  keys, and machine-specific paths before CI or release.

Do not attach OBS logs or HAR captures to public issues without reviewing and redacting them.
Use GitHub private vulnerability reporting for security disclosures. See [Security](SECURITY.md).

## Version

The current version is stored in [`VERSION`](VERSION). Builds, packages, and GitHub Releases all
derive their version from that file using semantic versioning.
