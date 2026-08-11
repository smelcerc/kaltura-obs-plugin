#!/usr/bin/env bash
set -euo pipefail

if pgrep -x obs >/dev/null 2>&1; then
  echo "Quit OBS Studio before uninstalling Kaltura Live." >&2
  exit 1
fi

if (( EUID != 0 )); then
  exec sudo "$0" "$@"
fi

if command -v dpkg-query >/dev/null 2>&1 &&
   dpkg-query -W -f='${db:Status-Status}' kaltura-live 2>/dev/null | grep -qx installed; then
  exec apt-get remove kaltura-live
fi

rm -f \
  /usr/lib/obs-plugins/libkaltura-live.so \
  /usr/lib/x86_64-linux-gnu/obs-plugins/libkaltura-live.so
rm -rf \
  /usr/share/obs/obs-plugins/kaltura-live \
  /usr/local/lib/obs-plugins/libkaltura-live.so \
  /usr/local/share/obs/obs-plugins/kaltura-live

echo "Kaltura Live was uninstalled."

