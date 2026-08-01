#!/usr/bin/env bash
set -euo pipefail

destination="${1:?usage: download-whisper-models.sh DESTINATION}"
mkdir -p "${destination}"

download_model() {
  local name="$1"
  local expected_sha256="$2"
  local target="${destination}/ggml-${name}.bin"
  local actual_sha256=""
  if [[ -f "${target}" ]]; then
    actual_sha256="$(shasum -a 256 "${target}" | awk '{print $1}')"
  fi
  if [[ "${actual_sha256}" == "${expected_sha256}" ]]; then
    return
  fi

  curl --fail --location --retry 3 --proto '=https' \
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-${name}.bin" \
    --output "${target}.download"
  actual_sha256="$(shasum -a 256 "${target}.download" | awk '{print $1}')"
  if [[ "${actual_sha256}" != "${expected_sha256}" ]]; then
    rm -f "${target}.download"
    echo "error: checksum verification failed for Local Whisper ${name}" >&2
    exit 1
  fi
  mv "${target}.download" "${target}"
}

download_model tiny.en 921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f
download_model base.en a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002
