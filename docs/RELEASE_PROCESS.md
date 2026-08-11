# Cross-platform release process

1. Update `VERSION`, release notes, OBS/Qt pins, dependency hashes, and supported-platform docs.
2. Run `scripts/audit-release.sh`, a clean configure/build, all unit tests, and package validation.
3. Push a pull request and require every independent CI job: macOS x86_64, macOS arm64, Universal
   merge, Windows x86_64, and Linux x86_64. One matrix failure does not cancel the others.
4. Download artifacts. Inspect `lipo -info`, `dumpbin /headers`, and `readelf -h`, then inspect
   dynamic libraries with `otool -L`, `dumpbin /dependents`, or `ldd`.
5. Run the full manual matrix in `CROSS_PLATFORM_AUDIT.md` on clean OBS profiles and all platforms.
6. Sign/notarize macOS deliverables when credentials are available. Signing is optional for local
   builds but required for normal public distribution. Add Windows signing when a certificate and
   trust process are established.
7. Generate SHA-256 manifests, create a signed semantic-version tag matching `VERSION`, publish all
   five named artifacts, and retain source/license notices.
8. Install each downloaded public artifact once more, verify OBS startup/exit, and monitor issues.

Never release historical files already in `dist/` without regenerating and validating them. Never
attach profiles, logs, KS values, stream keys, private keys, signing material, or notarization
credentials.
