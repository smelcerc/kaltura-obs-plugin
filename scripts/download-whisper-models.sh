#!/usr/bin/env bash
set -euo pipefail

destination="${1:?usage: download-whisper-models.sh DESTINATION}"
mkdir -p "${destination}"

sha256_file() {
  local path="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "${path}" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "${path}" | awk '{print $1}'
  elif command -v certutil >/dev/null 2>&1; then
    local native_path="${path}"
    if command -v cygpath >/dev/null 2>&1; then
      native_path="$(cygpath -w "${path}")"
    fi
    certutil -hashfile "${native_path}" SHA256 | sed -n '2{s/[[:space:]]//g;s/\r//g;p;}'
  else
    echo "error: no SHA-256 utility is available" >&2
    return 1
  fi
}

download_model() {
  local name="$1"
  local expected_sha256="$2"
  local target="${destination}/ggml-${name}.bin"
  local actual_sha256=""
  if [[ -f "${target}" ]]; then
    actual_sha256="$(sha256_file "${target}")"
  fi
  if [[ "${actual_sha256}" == "${expected_sha256}" ]]; then
    return
  fi

  curl --fail --location --retry 3 --proto '=https' \
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-${name}.bin" \
    --output "${target}.download"
  actual_sha256="$(sha256_file "${target}.download")"
  if [[ "${actual_sha256}" != "${expected_sha256}" ]]; then
    rm -f "${target}.download"
    echo "error: checksum verification failed for Local Whisper ${name}" >&2
    exit 1
  fi
  mv "${target}.download" "${target}"
}

download_model tiny.en 921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f
download_model base.en a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002
