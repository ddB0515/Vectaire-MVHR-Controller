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
START=$SECONDS
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

# Progress: numbered steps with the time since the start (set after the options are read)
STEP=0
TOTAL=5
[ "$BUILD" = 1 ] && TOTAL=6
step() { STEP=$((STEP + 1)); printf '\n[%d/%d] %s   (%ds)\n' "$STEP" "$TOTAL" "$*" $((SECONDS - START)); }

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
step "Finding the controller $HOST"
ADDR=$(resolve "$HOST")
if [ -z "$ADDR" ]; then
    echo "can't find $HOST: use its IP address instead (shown on the controller, in your router or on the setup page)" >&2
    is_wsl || echo "(for .local names on Linux, install avahi-daemon and libnss-mdns)" >&2
    exit 1
fi
[ "$ADDR" = "$HOST" ] || echo "    $HOST is $ADDR"

step "Device password"
if [ -z "$PASS" ]; then
    read -rsp "Device password for $HOST: " PASS
    echo
fi

if [ "$BUILD" = 1 ]; then
    step "Building the firmware (only changed files are rebuilt; the first build takes several minutes)"
    LOG=$(mktemp)
    set +e
    # show the compiler's progress lines ([12/37] Building ...) on one line, keep everything in the log
    tools/idf.sh "${VARIANT[@]}" build 2>&1 | tee "$LOG" | while IFS= read -r line; do
        case $line in
            \[[0-9]*/[0-9]*\]*)
                if [ -t 1 ]; then printf '\r\033[K    %s' "${line:0:110}"; else echo "    ${line:0:110}"; fi ;;
            *Configuring*|*"Generating"*|*"Linking"*|*"Project build complete"*) echo; echo "    ${line:0:110}" ;;
        esac
    done
    rc=${PIPESTATUS[0]}
    set -e
    echo
    if [ "$rc" != 0 ]; then
        echo "BUILD FAILED. The last lines of the build output:" >&2
        tail -n 40 "$LOG" >&2
        echo "(full log: $LOG)" >&2
        exit 1
    fi
    tail -n 3 "$LOG" | sed 's/^/    /'
    rm -f "$LOG"
fi

# build date/time from the image's app description (offset 32 + 80 / + 96)
built() { python3 -c 'import sys; d=open(sys.argv[1],"rb").read(256); t=lambda o: d[o:o+16].split(b"\0")[0].decode(); print(t(128), t(112))' "$1"; }
step "Checking the firmware image"
[ -f "$BIN" ] || { echo "    $BIN not found: build first (run without --no-build)" >&2; exit 1; }
WANT=$(built "$BIN")
echo "    $BIN: $(stat -c %s "$BIN") bytes, built $WANT"

step "Uploading to $HOST ($ADDR)"
# retry dropped connections, not refusals (curl exit 22 = HTTP error such as a wrong password)
for attempt in 1 2 3; do
    rc=0
    # the header comes from a pipe, so the password doesn't show up in the process list
    curl -fS $([ -t 2 ] && echo --progress-bar || echo -s) --max-time 300 -H @<(printf 'X-OTA-Password: %s\n' "$PASS") \
         -H "Content-Type: application/octet-stream" --data-binary @"$BIN" "http://$ADDR/api/ota" || rc=$?
    echo
    [ "$rc" = 0 ] && { echo "    upload finished, the controller checks and installs it"; break; }
    if [ "$rc" = 22 ]; then
        echo "upload refused: wrong password? It is on the board under Menu > Device > tap Firmware (headless: on the setup page, press the button for 1-4 s)."
        exit 1
    fi
    if [ "$attempt" = 3 ]; then
        echo "upload failed"
        exit 1
    fi
    echo "    connection problem, retrying in 5 s (attempt $attempt of 3) ..."
    sleep 5
done

step "Waiting for the controller to restart with the new firmware (up to 3 minutes)"
for i in $(seq 60); do
    sleep 3
    STATUS=$(curl -fsS --max-time 3 "http://$ADDR/api/status" 2>/dev/null || true)
    if [ -z "$STATUS" ]; then
        printf '    %3ds  not answering (restarting)\n' $((i * 3))
        continue
    fi
    GOT=$(python3 -c 'import json,sys; print(json.loads(sys.argv[1]).get("built",""))' "$STATUS" 2>/dev/null || true)
    if [ "$GOT" = "$WANT" ]; then
        echo "    running the new firmware: $STATUS"
        echo
        echo "Done in $((SECONDS - START)) s."
        exit 0
    fi
    printf '    %3ds  answering, still the old firmware (%s)\n' $((i * 3)) "$GOT"
done
echo "device didn't come back with the new firmware within 3 minutes (it may have rolled back)"
exit 1
