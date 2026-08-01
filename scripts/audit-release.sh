#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${project_dir}"

files=()
while IFS= read -r file; do
  files+=("${file}")
done < <(git ls-files --cached --others --exclude-standard)

if (( ${#files[@]} == 0 )); then
  echo "error: no release files found" >&2
  exit 1
fi

scan_files=()
for file in "${files[@]}"; do
  if [[ "${file}" != scripts/audit-release.sh ]]; then
    scan_files+=("${file}")
  fi
done

failed=false
for file in "${files[@]}"; do
  case "${file}" in
    *.har|*.log|*.pem|*.p12|*.key|*.mobileprovision|*.bin|.env|.env.*)
      echo "error: forbidden release file: ${file}" >&2
      failed=true
      ;;
  esac
done

patterns=(
  '-----BEGIN [A-Z ]*PRIVATE KEY-----'
  'djJ8[A-Za-z0-9_+=/-]{20,}'
  'rtmps?://[^[:space:]]+[?&](t|token|ks|key)='
  '/Users/[^/[:space:]]+/'
  '/home/[^/[:space:]]+/'
)
for pattern in "${patterns[@]}"; do
  matches="$(rg -l --no-messages --regexp "${pattern}" -- "${scan_files[@]}" || true)"
  if [[ -n "${matches}" ]]; then
    echo "error: sensitive-data pattern matched in:" >&2
    echo "${matches}" >&2
    failed=true
  fi
done

email_files="$(rg -l --no-messages --regexp '[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}' -- "${scan_files[@]}" || true)"
if [[ -n "${email_files}" ]]; then
  while IFS= read -r file; do
    if rg --no-messages --regexp '[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}' "${file}" \
      | rg -v 'example\.test|users\.noreply\.github\.com|security@example\.com' >/dev/null; then
      echo "error: non-example email address found in ${file}" >&2
      failed=true
    fi
  done <<< "${email_files}"
fi

if [[ "${failed}" == true ]]; then
  exit 1
fi
echo "Release audit passed for ${#files[@]} files."
