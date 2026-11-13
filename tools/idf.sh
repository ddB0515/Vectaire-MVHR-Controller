#!/usr/bin/env bash
# Run idf.py with the ESP-IDF version this project is pinned to (v6.1; v5.5.4 still works).
# Usage: tools/idf.sh [--idf 5.5.4] build | flash | monitor | ...                     display variant, build/
#        tools/idf.sh [--idf 5.5.4] --headless <target> build | flash | menuconfig ... headless, build-headless-<target>/
#        targets: esp32 esp32c3 esp32s3 esp32c5 esp32c6. The ESP-IDF version can also come from $VECTAIRE_IDF.
# Linux and Windows 11 (WSL 2, USB serial attached with usbipd): see README "Setting up" › "Windows 11 (WSL 2)".
set -e
cd "$(dirname "$0")/.."
idf=${VECTAIRE_IDF:-6.1}
dir=build
args=()
while [ $# -gt 0 ]; do
    case "$1" in
        --idf)
            idf=${2:-}
            shift 2 ;;
        --headless)
            target=${2:-}
            case "$target" in
                esp32|esp32c3|esp32s3|esp32c5|esp32c6) ;;
                *) echo "--headless needs a target: esp32 esp32c3 esp32s3 esp32c5 esp32c6" >&2; exit 2 ;;
            esac
            shift 2
            dir=build-headless-$target
            args=(-B "$dir" -D VECTAIRE_VARIANT=headless -D SDKCONFIG="$PWD/$dir/sdkconfig")
            export IDF_TARGET=$target ;;
        *) break ;;
    esac
done
is_wsl() { grep -qi microsoft /proc/sys/kernel/osrelease 2>/dev/null; }

activate="$HOME/.espressif/tools/activate_idf_v$idf.sh"
active=""      # ESP-IDF already set up in this shell (CI: the espressif/idf Docker image), if it is the right version
if [ ! -f "$activate" ] && [ -n "${IDF_PATH:-}" ] && command -v idf.py >/dev/null; then
    h=$IDF_PATH/components/esp_common/include/esp_idf_version.h
    v="$(awk '/define ESP_IDF_VERSION_MAJOR/ {print $3}' "$h").$(awk '/define ESP_IDF_VERSION_MINOR/ {print $3}' "$h")"
    if [ "$v" = "$idf" ] || [ "$v.$(awk '/define ESP_IDF_VERSION_PATCH/ {print $3}' "$h")" = "$idf" ]; then
        active=1
    else
        echo "the active ESP-IDF ($IDF_PATH) is v$v, this project wants v$idf" >&2
        exit 2
    fi
fi
if [ ! -f "$activate" ] && [ -z "$active" ]; then
    echo "ESP-IDF v$idf is not installed ($activate missing). Install it with the ESP-IDF Installation Manager:" >&2
    echo "    eim install -i v$idf" >&2
    exit 2
fi

# A serial port that isn't there: say why instead of letting esptool fail. Under WSL the board must be attached first.
for ((i = 1; i <= $#; i++)); do
    if [ "${!i}" = "-p" ] || [ "${!i}" = "--port" ]; then
        j=$((i + 1))
        port=${!j:-}
        if [ -n "$port" ] && [ ! -e "$port" ]; then
            echo "serial port $port not found." >&2
            if is_wsl; then
                echo "Under WSL, attach the board first, in a Windows terminal (PowerShell):" >&2
                echo "    usbipd list                      (find the board: CH340 / CP210x / USB JTAG)" >&2
                echo "    usbipd attach --wsl --busid <BUSID>" >&2
                echo "Then it shows up here as /dev/ttyUSB0 or /dev/ttyACM0 (ls /dev/ttyUSB* /dev/ttyACM*)." >&2
            else
                echo "Connected boards: $(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | tr '\n' ' ')" >&2
            fi
            exit 2
        fi
    fi
done

# A build folder configured by another ESP-IDF version (or VS Code with another one) breaks the build in odd ways
# (wrong compiler, missing headers). Start it over, keeping its sdkconfig (the headless variant keeps it there).
marker=$dir/.vectaire-idf
if [ -f "$dir/CMakeCache.txt" ] && [ "$(cat "$marker" 2>/dev/null)" != "$idf" ]; then
    echo "$dir was configured by another ESP-IDF version: starting it over for v$idf (sdkconfig kept)" >&2
    find "$dir" -mindepth 1 -maxdepth 1 ! -name sdkconfig -exec rm -rf {} +
fi
mkdir -p "$dir"
echo "$idf" > "$marker"

if [ -n "$active" ]; then
    exec idf.py "${args[@]}" "$@"
fi
while IFS='=' read -r key value; do
    [ -n "$key" ] && export "$key=$value"
done < <(bash "$activate" -e)
export PATH="$PATH:$SYSTEM_PATH"
exec "$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" "${args[@]}" "$@"
