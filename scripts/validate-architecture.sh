#!/usr/bin/env bash
set -euo pipefail

if (( $# != 3 )); then
  echo "usage: $0 <macos|linux> <binary> <expected-architecture>" >&2
  exit 2
fi

platform="$1"
binary="$2"
expected="$3"
if [[ ! -f "${binary}" ]]; then
  echo "error: binary not found: ${binary}" >&2
  exit 1
fi

file "${binary}"
case "${platform}" in
  macos)
    actual="$(lipo -archs "${binary}")"
    lipo -info "${binary}"
    for architecture in ${expected//;/ }; do
      if [[ " ${actual} " != *" ${architecture} "* ]]; then
        echo "error: expected ${architecture}; found: ${actual}" >&2
        exit 1
      fi
    done
    ;;
  linux)
    readelf -h "${binary}"
    if [[ "${expected}" == x86_64 ]] &&
       ! readelf -h "${binary}" | grep -Eq 'Machine:[[:space:]]+(Advanced Micro Devices X86-64|AMD x86-64)'; then
      echo "error: expected an x86_64 ELF binary" >&2
      exit 1
    fi
    ;;
  *)
    echo "error: unsupported platform: ${platform}" >&2
    exit 2
    ;;
esac
