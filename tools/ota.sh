#!/usr/bin/env bash
# Build the firmware and install it over Wi-Fi.
# Usage: tools/ota.sh <host> [--no-build] [-p PASSWORD] [--headless TARGET]   (host: name or IP, or $VECTAIRE_HOST)
# The device password is on the board: Menu > Device > tap the Firmware row (headless: on the setup page, press the
# button for 1-4 s). Without -p (or $VECTAIRE_OTA_PASSWORD) it is asked for; typing it at the prompt keeps it out of
# the shell history. --headless esp32c3 etc. builds and sends the headless variant for that chip.
# Linux and Windows 11 (WSL 2): under WSL, .local names that WSL can't resolve are looked up through Windows.
set -euo pipefail
cd "$(dirname "$0")/.."

HOST=${VECTAIRE_HOST:-}
BUILD=1
PASS=${VECTAIRE_OTA_PASSWORD:-}
VARIANT=()
while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) BUILD=0 ;;
        -p|--password)
            [ $# -ge 2 ] || { echo "$1 needs a value" >&2; exit 2; }
            PASS=$2
            shift ;;
        --password=*) PASS=${1#--password=} ;;
        --headless)
            [ $# -ge 2 ] || { echo "$1 needs a target (esp32 esp32c3 esp32s3 esp32c5 esp32c6)" >&2; exit 2; }
            VARIANT=(--headless "$2")
            shift ;;
        -h|--help) sed -n '2,6p' "$0"; exit 0 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) HOST=$1 ;;
    esac
    shift
done
if [ ${#VARIANT[@]} -gt 0 ]; then
    BIN=build-headless-${VARIANT[1]}/vectaire_headless.bin
else
    BIN=build/vectaire_c5.bin
fi

if [ -z "$HOST" ]; then
    echo "which controller? tools/ota.sh vectaire-xxxx.local (or its IP), or set \$VECTAIRE_HOST" >&2
    exit 2
fi

# The controller's address. WSL 2 (NAT networking) usually can't resolve mDNS .local names, Windows can.
is_wsl() { grep -qi microsoft /proc/sys/kernel/osrelease 2>/dev/null; }
resolve() {
    if [[ $1 =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        echo "$1"
    elif getent ahostsv4 "$1" >/dev/null 2>&1; then
        getent ahostsv4 "$1" | awk 'NR==1 {print $1}'
    elif is_wsl && command -v powershell.exe >/dev/null && [[ $1 =~ ^[A-Za-z0-9.-]+$ ]]; then
        # .NET goes through Windows' own resolver, which answers .local names via mDNS
        powershell.exe -NoProfile -NonInteractive -Command \
            "[System.Net.Dns]::GetHostAddresses('$1') | Where-Object AddressFamily -eq 'InterNetwork' | Select-Object -First 1 | ForEach-Object IPAddressToString" \
            2>/dev/null | tr -d '\r' | grep -E '^[0-9.]+$' || true
    fi
    return 0
}
ADDR=$(resolve "$HOST")
if [ -z "$ADDR" ]; then
    echo "can't find $HOST: use its IP address instead (shown on the controller, in your router or on the setup page)" >&2
    is_wsl || echo "(for .local names on Linux, install avahi-daemon and libnss-mdns)" >&2
    exit 1
fi
[ "$ADDR" = "$HOST" ] || echo "$HOST is $ADDR"

if [ -z "$PASS" ]; then
    read -rsp "Device password for $HOST: " PASS
    echo
fi

if [ "$BUILD" = 1 ]; then
    tools/idf.sh "${VARIANT[@]}" build | tail -n 3
fi

# build date/time from the image's app description (offset 32 + 80 / + 96)
built() { python3 -c 'import sys; d=open(sys.argv[1],"rb").read(256); t=lambda o: d[o:o+16].split(b"\0")[0].decode(); print(t(128), t(112))' "$1"; }
WANT=$(built "$BIN")

echo "uploading $BIN ($(stat -c %s "$BIN") bytes, built $WANT) to $HOST ..."
# retry dropped connections, not refusals (curl exit 22 = HTTP error such as a wrong password)
for attempt in 1 2 3; do
    rc=0
    # the header comes from a pipe, so the password doesn't show up in the process list
    curl -fsS --max-time 300 -H @<(printf 'X-OTA-Password: %s\n' "$PASS") \
         -H "Content-Type: application/octet-stream" --data-binary @"$BIN" "http://$ADDR/api/ota" || rc=$?
    echo
    [ "$rc" = 0 ] && break
    if [ "$rc" = 22 ]; then
        echo "upload refused: wrong password? It is on the board under Menu > Device > tap Firmware (headless: on the setup page, press the button for 1-4 s)."
        exit 1
    fi
    if [ "$attempt" = 3 ]; then
        echo "upload failed"
        exit 1
    fi
    echo "connection problem, retrying in 5 s ..."
    sleep 5
done

echo -n "waiting for the device to restart"
for _ in $(seq 60); do
    sleep 3
    echo -n .
    STATUS=$(curl -fsS --max-time 3 "http://$ADDR/api/status" 2>/dev/null || true)
    if [ -n "$STATUS" ] && [ "$(python3 -c 'import json,sys; print(json.loads(sys.argv[1]).get("built",""))' "$STATUS")" = "$WANT" ]; then
        echo
        echo "running the new firmware: $STATUS"
        exit 0
    fi
done
echo
echo "device didn't come back with the new firmware within 3 minutes (it may have rolled back)"
exit 1
