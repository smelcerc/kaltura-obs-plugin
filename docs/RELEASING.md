# Release Process

## Versioning

The project uses semantic versioning. Update `VERSION` only:

- Patch: compatible fixes
- Minor: compatible features
- Major: incompatible behavior or configuration changes

CMake, compiled metadata, package names, Info.plist, and GitHub Releases derive from this value.

## Local validation

```bash
./scripts/audit-release.sh
cmake --build build --parallel
ctest --test-dir build --output-on-failure
DIST_DIR="$PWD/dist" ./scripts/package-macos.sh
```

On Linux, run `./scripts/package-linux.sh`. Inspect package contents before publishing:

```bash
pkgutil --payload-files dist/*.pkg
dpkg-deb --contents dist/*.deb
tar -tzf dist/*.tar.gz
```

Confirm that no artifact contains settings, logs, HAR files, KS values, stream keys, certificates,
or absolute developer paths.

## GitHub release

1. Update `VERSION` and complete `RELEASE_NOTES_TEMPLATE.md`.
2. Confirm CI succeeds on `main`.
3. Create and push a matching tag, for example `v0.1.1`.
4. The Release workflow builds macOS and Linux packages, downloads verified Whisper models,
   generates checksums and attestations, and publishes the GitHub Release with its installers.
5. Confirm the release page lists the `.pkg`, `.deb`, archives, checksums, and attestations.

## macOS signing placeholders

Unsigned CI/local packages use ad-hoc plugin signing. For Developer ID signing, configure these
GitHub Actions secrets:

- `MACOS_CERTIFICATE_BASE64`
- `MACOS_CERTIFICATE_PASSWORD`
- `MACOS_APPLICATION_SIGNING_IDENTITY`
- `MACOS_INSTALLER_SIGNING_IDENTITY`
- `APPLE_NOTARY_USERNAME`
- `APPLE_NOTARY_PASSWORD`
- `APPLE_TEAM_ID`

The certificate must contain the Developer ID Application and Installer identities referenced by
the identity secrets. Secrets are imported into an ephemeral runner keychain. Notarization is
enabled only when the notary username is configured.

## Public-release checklist

- [ ] Product ownership and repository license have been approved and a `LICENSE` file added.
- [ ] Version and tag match.
- [ ] Release notes describe user-visible changes and known issues.
- [ ] Secret audit and all tests pass.
- [ ] macOS package is signed/notarized or clearly labeled unsigned.
- [ ] Linux package installs and OBS loads it on a clean supported system.
- [ ] Tiny and Base model checksums match pinned values.
- [ ] SHA-256 checksums and GitHub provenance attestations are present.
- [ ] Upgrade, uninstall, Primary/Backup, captions, and rollback paths were smoke-tested.
