#!/bin/bash

set -e -o pipefail

MOUNT_TARGET="/usr/palm/applications/com.webos.app.screensaver/qml/main.qml"
ASSETS="$(dirname "$(realpath "$0")")"
QML_PATH="$ASSETS/screensaver-main.qml"
NYX_INFO="/var/run/nyx/os_info.json"

if [[ ! -f "$MOUNT_TARGET" ]]; then
    echo "[-] Target file does not exist: $MOUNT_TARGET" >&2
    exit 1
fi

if [[ ! -f "$QML_PATH" ]]; then
    echo "[-] QML does not exist: $QML_PATH" >&2
    exit 1
fi

# webOS major for the screensaver. /var/run/nyx/os_info.json is the source
# that answered on device (webos_release). luna systemproperty returned an
# empty body. A missing or unparseable release is a hard error here so we
# do not bind a QML that would guess.
if [[ ! -f "$NYX_INFO" ]]; then
    echo "[-] Missing $NYX_INFO" >&2
    exit 1
fi

RELEASE="$(sed -n 's/.*"webos_release"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$NYX_INFO" | head -1)"
MAJOR="${RELEASE%%.*}"
case "$MAJOR" in
    ''|*[!0-9]*)
        echo "[-] Could not parse webos_release from $NYX_INFO (got '${RELEASE}')" >&2
        exit 1
        ;;
esac

cat > "$ASSETS/platform.js" <<EOF
var WEBOS_RELEASE = "$RELEASE";
var WEBOS_MAJOR = $MAJOR;
EOF
echo "[+] platform.js webOS $RELEASE" >&2

if findmnt "$MOUNT_TARGET" >/dev/null 2>&1; then
    echo "[*] Rebinding screensaver QML" >&2
    if ! umount "$MOUNT_TARGET"; then
        echo "[-] umount failed: $MOUNT_TARGET" >&2
        exit 1
    fi
fi

mount --bind "$QML_PATH" "$MOUNT_TARGET"
findmnt -n -o SOURCE,TARGET "$MOUNT_TARGET"
echo "[+] Enabled successfully" >&2
