#!/bin/bash
set -euo pipefail

if /usr/bin/pgrep -x OBS >/dev/null 2>&1; then
  echo "Quit OBS Studio before uninstalling Kaltura Live." >&2
  exit 1
fi

if (( EUID != 0 )); then
  echo "Administrator access is required to remove the system installation."
  exec /usr/bin/sudo "$0" "$@"
fi

console_user="$(/usr/bin/stat -f '%Su' /dev/console)"
if [[ -z "${console_user}" || "${console_user}" == root || "${console_user}" == loginwindow ]]; then
  echo "A desktop user must be signed in to uninstall Kaltura Live." >&2
  exit 1
fi

user_home="$(/usr/bin/dscl . -read "/Users/${console_user}" NFSHomeDirectory | /usr/bin/awk '{print $2}')"
if [[ -z "${user_home}" || ! -d "${user_home}" ]]; then
  echo "Could not resolve the signed-in user's home directory." >&2
  exit 1
fi

/bin/rm -rf \
  "/Library/Application Support/obs-studio/plugins/kaltura-live.plugin" \
  "${user_home}/Library/Application Support/obs-studio/plugins/kaltura-live.plugin"
/bin/rm -f "/Applications/Uninstall Kaltura Live.command"

echo "Kaltura Live was uninstalled."

