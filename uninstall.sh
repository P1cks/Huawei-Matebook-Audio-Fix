#!/usr/bin/env bash
# Remove the fix and roll back to the stock driver.
# Usage: sudo ./uninstall.sh
set -euo pipefail

PACKAGE="huawei-matebook-audio-fix"
VERSION="1.0"

[ "$EUID" -ne 0 ] && { echo "Run as root: sudo ./uninstall.sh" >&2; exit 1; }

if dkms status "$PACKAGE/$VERSION" 2>/dev/null | grep -q "$PACKAGE/$VERSION"; then
    echo "Removing DKMS module..."
    dkms remove "$PACKAGE/$VERSION" --all
else
    echo "Module is not installed."
fi

depmod -a
echo "Done. Reboot to return to the stock driver."
