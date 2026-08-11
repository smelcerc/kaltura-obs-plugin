#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-${project_dir}/build-linux}"
dist_dir="${DIST_DIR:-${project_dir}/dist}"
package_output_dir="${build_dir}/cpack-output"
model_dir="${KALTURA_LIVE_MODEL_SOURCE_DIR:-${build_dir}/release-models}"
obs_version="${OBS_VERSION:-32.1.2}"
obs_source="${KALTURA_LIVE_OBS_SOURCE_PATH:-${project_dir}/third_party/obs-studio}"

for command in cmake cpack curl file git ninja shasum; do
  if ! command -v "${command}" >/dev/null 2>&1; then
    echo "error: required Linux packaging command not found: ${command}" >&2
    exit 1
  fi
done

if [[ ! -f "${obs_source}/libobs/obs-module.h" ]]; then
  git clone --depth 1 --branch "${obs_version}" \
    https://github.com/obsproject/obs-studio.git "${obs_source}"
fi
"${project_dir}/scripts/download-whisper-models.sh" "${model_dir}"
cmake -S "${project_dir}" -B "${build_dir}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DKALTURA_LIVE_OBS_SOURCE_PATH="${obs_source}" \
  -DKALTURA_LIVE_MODEL_SOURCE_DIR="${model_dir}"
cmake --build "${build_dir}" --parallel
"${project_dir}/scripts/validate-architecture.sh" linux \
  "${build_dir}/libkaltura-live.so" x86_64
ctest --test-dir "${build_dir}" --output-on-failure
mkdir -p "${dist_dir}" "${package_output_dir}"
(cd "${build_dir}" && cpack -B "${package_output_dir}")
shopt -s nullglob
packages=("${package_output_dir}"/*.deb "${package_output_dir}"/*.tar.gz)
if (( ${#packages[@]} == 0 )); then
  echo "error: CPack did not produce a Linux package" >&2
  exit 1
fi
cp -f "${packages[@]}" "${dist_dir}/"
(cd "${dist_dir}" && shasum -a 256 ./*.deb ./*.tar.gz > SHA256SUMS-linux.txt)
