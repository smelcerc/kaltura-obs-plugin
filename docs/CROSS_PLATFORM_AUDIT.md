# Cross-platform audit

Audit date: 2026-08-10. The repository targets OBS Studio 32.0 or newer and CI pins OBS 32.1.2.

## Findings and remediation

| Area | Finding | Remediation |
|---|---|---|
| Build system | The single CMake file contained dependency acquisition, macOS fallback linking, install rules, and packaging. Non-Apple fallback ended in a fatal error. | The root remains the only entry point and explicitly dispatches `APPLE`, `WIN32`, and Linux to `cmake/macos.cmake`, `windows.cmake`, and `linux.cmake`. Shared discovery is in `dependencies.cmake`. Missing OBS headers/libraries produce actionable errors. |
| OBS SDK | macOS linked directly to `/Applications/OBS.app`; Windows had no discovery route. | Packaged CMake targets are preferred. Cache variables allow an explicit OBS application, SDK prefix, or individual libraries. The application path is a configurable macOS development fallback only. |
| Architecture | CI and setup accepted Intel only. Packaging names did not record architecture. | Native x86_64 and arm64 builds are separate CI jobs and run `lipo` validation. A merge job creates and validates Universal 2 output. Windows uses `dumpbin`; Linux uses `file` and `readelf`. |
| Universal builds | whisper.cpp v1.9.1 chooses a CPU backend when CMake configures and cannot safely produce both CPU slices in one build tree. Thin OBS application frameworks also cannot link the opposite slice. | Direct multi-value `CMAKE_OSX_ARCHITECTURES` fails with an explanation. Build native slices against matching OBS runtimes and merge with `package-macos-universal.sh`. Long term, use a universal OBS SDK and make ggml select per-architecture flags at compile time. |
| Homebrew | Scripts defaulted to `/usr/local/opt/qtbase`, an Intel-only assumption. | Scripts use `QT_PREFIX` or `brew --prefix qt`; neither `/usr/local` nor `/opt/homebrew` is embedded in build configuration. |
| Qt | Qt was found globally but the API client manually copied include properties. Runtime TLS loading was embedded in plugin business logic. | Imported Qt6 targets are used throughout. Qt 6.8+ is required to match OBS 32. Runtime bundle discovery is in the platform layer. macOS packaging verifies that development and OBS runtime Qt versions match. |
| KS persistence | `SettingsManager` wrote the Kaltura Session into OBS profile JSON as plaintext. | `CredentialStore` uses macOS Keychain, Windows Credential Manager, and Secret Service/libsecret. Each OBS settings record keeps only an opaque credential UUID, preserving per-record separation. Legacy plaintext is read once, migrated, and omitted on every subsequent save. No insecure fallback is implemented and no secret is logged. |
| Filesystem paths | macOS bundle traversal was in `Plugin`; model data behavior differed implicitly. | `platform::runtimePaths` hides bundle versus OBS module-data layout. Qt `QDir` keeps native separators and UTF handling. |
| Networking | All HTTP/TLS work uses Qt Network. No POSIX sockets, platform certificate paths, or native network APIs were found. | Retained as platform-neutral. Qt uses the operating system/runtime TLS backend. |
| Threading/timers | Captions use C++20 threads, atomics, mutexes and condition variables; UI timing uses Qt. No pthread calls were found. | Retained. These primitives are supported by AppleClang, MSVC, GCC, and Clang. |
| Dynamic loading | No direct `dlopen`, `LoadLibrary`, or platform loader calls were found. | OBS loads the module. macOS Qt TLS plug-in discovery is isolated behind runtime paths. |
| Objective-C | No Objective-C source existed. | The macOS platform implementation is Objective-C++ only as a platform compilation boundary and calls the Security framework C API. Business logic stays C++. |
| Captions | whisper.cpp was fetched at a fixed tag; OBS's libcaption was compiled directly from an assumed source checkout. CPU-native optimization could create nonportable artifacts. | The tag remains pinned, `GGML_NATIVE` is off, and model files remain external package data. CMake validates the OBS source tree containing libcaption. Universal limitations are explicit. |
| Bundled libraries | No committed random binary libraries were found. `dist/` contains prior release packages, while build trees contain fetched whisper sources. | New builds fetch a pinned source tag and use official OBS dependencies/system packages. Release audit continues to reject machine-specific and sensitive inputs. Existing historical artifacts should be regenerated before the next release. |
| Install layout | One non-Apple rule placed DLLs under a Unix library directory; no Windows package existed. | Windows installs to `obs-plugins/64bit` and `data/obs-plugins/kaltura-live`; Linux uses GNU library/share paths; macOS uses an OBS `.plugin` bundle. |
| Lifecycle | OBS outputs and services are reference-counted manually; callbacks and timers are removed during shutdown. | Existing architecture is preserved. Shutdown stops captions/outputs before destroying Qt/API objects, releases saved OBS settings, and invalidates asynchronous callbacks. Manual exit testing remains required. |
| OBS APIs | The plugin uses libobs and the frontend API, including docks, output captions, services and raw-audio callbacks. No deprecated function found in plugin code during this audit. | Minimum OBS is 32.0 because OBS 32 rejects plug-ins built for newer ABI versions and this project is validated against the OBS 32 line. Compile against the oldest supported SDK for releases. |

## Platform-neutral code retained

The Kaltura client, HTTP transport, entry models, output routing, caption manager, Whisper provider,
CEA-608 construction, settings UI, dock, and logging contain no OS-specific APIs. `std::filesystem`
is used only for model-file validation and is supported by all selected C++20 toolchains.

## Remaining release risks

- Native Windows and Apple Silicon runtime tests require those hosts; an Intel development machine
  cannot prove OBS loading on them.
- Universal output is a merge of independently built slices, not a single CMake invocation, due to
  whisper.cpp/ggml CPU configuration and thin OBS application frameworks.
- Linux requires libsecret at build/runtime and a running Secret Service provider to persist a KS.
  A locked or unavailable service leaves a newly entered KS available only for the current process.
- Whisper model files are large external release inputs. Their pinned checksums and licensing must
  be reviewed whenever models or whisper.cpp are updated.

## Manual runtime matrix

Run every row on macOS Intel, macOS Apple Silicon, Windows x86_64, and Ubuntu 24.04 x86_64. Also run
the macOS row with the Universal bundle.

1. Launch OBS and confirm the module loads without architecture or Qt errors.
2. Open the dock and settings UI; validate a KS and confirm it is absent from OBS profile JSON/logs.
3. Restart OBS and confirm secure KS retrieval and non-secret settings persistence.
4. Load/search live entries and thumbnails; exercise invalid/expired KS and offline/time-out paths.
5. Apply and revert stream settings; start/stop Primary, Backup, and Both modes.
6. Confirm health, reconnect, dropped-frame, bitrate, and latency displays during failures.
7. Run Tiny and Base caption models; verify preview, delay, placement, alignment, dictionary, and
   CEA-608 delivery.
8. Stop streams, switch profiles/scene collections, restart, then exit OBS normally. Check for
   leaked outputs, callbacks, Qt objects, hangs, or crash recovery prompts.
