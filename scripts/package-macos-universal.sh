#!/usr/bin/env bash
set -euo pipefail

if (( $# != 3 )); then
  echo "usage: $0 <x86_64.plugin> <arm64.plugin> <output-directory>" >&2
  exit 2
fi

x64_bundle="$1"
arm_bundle="$2"
dist_dir="$3"
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="$(tr -d '[:space:]' < "${project_dir}/VERSION")"
staging_dir="$(mktemp -d /tmp/kaltura-live-universal.XXXXXX)"
trap 'rm -rf "${staging_dir}"' EXIT

bundle="${staging_dir}/kaltura-live.plugin"
ditto "${arm_bundle}" "${bundle}"
lipo -create \
  "${x64_bundle}/Contents/MacOS/kaltura-live" \
  "${arm_bundle}/Contents/MacOS/kaltura-live" \
  -output "${bundle}/Contents/MacOS/kaltura-live"

for relative in Contents/PlugIns/tls/libqsecuretransportbackend.dylib; do
  if [[ -f "${x64_bundle}/${relative}" && -f "${arm_bundle}/${relative}" ]]; then
    bundled_architectures="$(lipo -archs "${bundle}/${relative}")"
    if [[ " ${bundled_architectures} " != *" x86_64 "* ||
          " ${bundled_architectures} " != *" arm64 "* ]]; then
      lipo -create "${x64_bundle}/${relative}" "${arm_bundle}/${relative}" \
        -output "${bundle}/${relative}"
    fi
  fi
done

"${project_dir}/scripts/validate-architecture.sh" macos \
  "${bundle}/Contents/MacOS/kaltura-live" 'x86_64;arm64'
"${project_dir}/scripts/validate-architecture.sh" macos \
  "${bundle}/Contents/PlugIns/tls/libqsecuretransportbackend.dylib" 'x86_64;arm64'
codesign --force --deep --sign "${MACOS_APPLICATION_SIGNING_IDENTITY:--}" \
  --timestamp=none "${bundle}"
mkdir -p "${dist_dir}"
archive="${dist_dir}/kaltura-live-${version}-macOS-universal.tar.gz"
COPYFILE_DISABLE=1 tar -C "${staging_dir}" -czf "${archive}" kaltura-live.plugin
shasum -a 256 "${archive}" > "${archive}.sha256"
echo "Created ${archive}"
