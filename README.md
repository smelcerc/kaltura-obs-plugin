# Kaltura Live for OBS Studio

Kaltura Live is an OBS Studio 31+ plugin for selecting Kaltura live entries, configuring Primary
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

Download the package for your operating system from GitHub Releases:

- **macOS:** open `kaltura-live-VERSION-macos.pkg`. The portable `.tar.gz` can be extracted and
  copied manually instead.
- **Ubuntu/Debian:** `sudo apt install ./kaltura-live_VERSION_amd64.deb`.
- **Other Linux:** extract the Linux `.tar.gz` under `/` using your distribution's packaging
  conventions.

Restart OBS, then open **Tools → Kaltura Live Settings…**. See the
[User Guide](docs/USER_GUIDE.md) for setup and operation.

## Build and contribute

See [Developer Documentation](docs/DEVELOPMENT.md), [Contributing](CONTRIBUTING.md), and
[Release Process](docs/RELEASING.md).

```bash
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/path/to/qt6
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For local macOS development, `./scripts/deploy-local-macos.sh` configures, builds, validates Qt/OBS
runtime compatibility, and deploys to the current user's OBS plugin directory.

## Security and privacy

- KS values and stream credentials are masked and never intentionally logged.
- Transcription runs locally through `whisper.cpp`.
- Release models are downloaded over HTTPS and verified against pinned SHA-256 checksums.
- `./scripts/audit-release.sh` rejects common secret files, KS values, stream-token URLs, private
  keys, and machine-specific paths before CI or release.

Do not attach OBS logs or HAR captures to public issues without reviewing and redacting them.
Use GitHub private vulnerability reporting for security disclosures. See [Security](SECURITY.md).

## Version

The current version is stored in [`VERSION`](VERSION). Builds, packages, and GitHub Releases all
derive their version from that file using semantic versioning.
