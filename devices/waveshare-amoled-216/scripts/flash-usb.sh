#!/usr/bin/env bash
#
# flash-usb.sh — one-time USB flash for a Waveshare ESP32-S3-Touch-AMOLED-2.16
# running this config: installs the custom partition table (partitions.csv,
# which shrinks the OTA slots and adds the 2 MB `model` partition) and writes
# the ESP-SR model blob into that partition.
#
# Why USB: ESPHome's UI/OTA can flash bootloader / partition-table / app, but
# never a data partition. The factory image ESPHome builds contains only a
# 4-byte placeholder for `model` (that stub comes from the esp-sr component),
# so the real models must be written separately. After this one flash every
# update stays OTA and leaves the model partition alone.
#
# Usage:
#   bash scripts/flash-usb.sh                                   # default config, /dev/ttyACM0
#   bash scripts/flash-usb.sh /dev/ttyUSB0                      # another port
#   bash scripts/flash-usb.sh --config waveshare-amoled-216-2   # a cloned config
#   bash scripts/flash-usb.sh --dry-run                         # print, do not write
#
# Run it either (a) on a machine with esptool (`source ~/code/esp-idf/export.sh`),
# or (b) without esptool, inside the esphome image with the board passed through:
#
#   docker run --rm -it --user 0:0 --device /dev/ttyACM0 \
#     -v "$PWD/config/esphome/config":/config esphome/esphome:latest \
#     bash /config/scripts/flash-usb.sh /dev/ttyACM0
#
# (See docs/WAVESHARE_AMOLED_216_NEW_DEVICE.md for the full new-device recipe.)
#
# What it writes (nothing else is touched — nvs at 0xF90000 keeps WiFi creds):
#   0x0       bootloader.bin        (from the ESPHome build)
#   0x8000    partition-table.bin   (the 6-partition layout in partitions.csv)
#   0x9000    ota_data_initial.bin  (blank otadata -> boots app0)
#   0x10000   <app>.bin             (app; the name is read from the build)
#   <model>   srmodels.bin          (offset read from partitions.csv)
#
# The image list and offsets come from the build's own `flash_args`, so a
# renamed or cloned config works without editing this script.
#
set -euo pipefail

die() { echo "error: $*" >&2; exit 1; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"        # this device's folder in the repo
PARTITIONS_CSV="${DEVICE_DIR}/partitions.csv"
MODELS="${DEVICE_DIR}/srmodels.bin"

CONFIG_NAME="waveshare-amoled-216"
BUILD_ROOT="${ESPHOME_BUILD_ROOT:-/config/.esphome/build}"
BUILD_DIR=""
PORT="/dev/ttyACM0"
DRY_RUN=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run)       DRY_RUN=1; shift ;;
    --config)        CONFIG_NAME="${2:?--config needs a config name}"; shift 2 ;;
    --config=*)      CONFIG_NAME="${1#*=}"; shift ;;
    --build-root)    BUILD_ROOT="${2:?--build-root needs a path}"; shift 2 ;;
    --build-root=*)  BUILD_ROOT="${1#*=}"; shift ;;
    --build-dir)     BUILD_DIR="${2:?--build-dir needs a path}"; shift 2 ;;
    --build-dir=*)   BUILD_DIR="${1#*=}"; shift ;;
    --models)        MODELS="${2:?--models needs a path}"; shift 2 ;;
    --models=*)      MODELS="${1#*=}"; shift ;;
    --port)          PORT="${2:?--port needs a device node}"; shift 2 ;;
    --port=*)        PORT="${1#*=}"; shift ;;
    -h|--help)       sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    /*)              PORT="$1"; shift ;;
    *)               die "unknown argument: $1 (try --help)" ;;
  esac
done

if [[ "${DRY_RUN}" == "0" && ! -e "${PORT}" ]]; then
  echo "error: ${PORT} does not exist on this machine." >&2
  echo "       Plug the board in (it shows up as an Espressif USB-Serial-JTAG or a" >&2
  echo "       CP210x/CH34x bridge) and pass the port, e.g.: bash $0 /dev/ttyACM1" >&2
  echo "       Available:" >&2
  ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null >&2 || echo "         (none)" >&2
  exit 1
fi

# The build dir is where ESPHome compiled the config: <build-root>/<config>/build.
# --build-dir wins (point it at a downloaded/copied build tree, e.g. the folder
# that contains flash_args, bootloader/ and partition_table/).
if [[ -z "${BUILD_DIR}" ]]; then
  BUILD_DIR="${BUILD_ROOT}/${CONFIG_NAME}/build"
fi
FLASH_ARGS="${BUILD_DIR}/flash_args"

[[ -d "${BUILD_DIR}" ]] || die "no build dir at ${BUILD_DIR}
       compile first:  esphome compile /config/${CONFIG_NAME}.yaml
       or pass --build-dir <dir containing flash_args>"
[[ -f "${MODELS}" ]] || die "missing ${MODELS}
       (it ships in this repository; regenerate with bash ${SCRIPT_DIR}/pack-models.sh)"

MODEL_OFFSET="$(awk -F, '/^[[:space:]]*model[[:space:]]*,/ {gsub(/[[:space:]]/,"",$4); print $4}' "${PARTITIONS_CSV}" | head -1)"
MODEL_OFFSET="${MODEL_OFFSET:-0xD90000}"

# --- image list: prefer the build's own flash_args --------------------------
IMAGES=()   # alternating: offset path offset path ...
if [[ -f "${FLASH_ARGS}" ]]; then
  while read -r off path; do
    [[ -z "${off:-}" ]] && continue
    [[ "${off}" == -* ]] && continue            # the "--flash_mode dio ..." options line
    [[ "${path}" == *srmodels* ]] && continue   # esp-sr's 4-byte placeholder, replaced below
    IMAGES+=("${off}" "${BUILD_DIR}/${path}")
  done < "${FLASH_ARGS}"
fi
if [[ ${#IMAGES[@]} -eq 0 ]]; then
  # Fallback for a build without flash_args (older ESPHome, Arduino framework)
  IMAGES=(0x0       "${BUILD_DIR}/bootloader/bootloader.bin"
          0x8000    "${BUILD_DIR}/partition_table/partition-table.bin"
          0x9000    "${BUILD_DIR}/ota_data_initial.bin"
          0x10000   "${BUILD_DIR}/${CONFIG_NAME}.bin")
fi
IMAGES+=("${MODEL_OFFSET}" "${MODELS}")

for ((i = 1; i < ${#IMAGES[@]}; i += 2)); do
  f="${IMAGES[$i]}"
  [[ -f "$f" ]] || die "missing ${f}
       compile first:  esphome compile /config/${CONFIG_NAME}.yaml"
done

# --- esptool ---------------------------------------------------------------
if [[ -x "$(command -v esptool 2>/dev/null)" ]]; then
  ESPTOOL=(esptool)
elif [[ -x "$(command -v esptool.py 2>/dev/null)" ]]; then
  ESPTOOL=(esptool.py)
elif python3 -c "import esptool" 2>/dev/null; then
  ESPTOOL=(python3 -m esptool)
elif [[ "${DRY_RUN}" == "1" ]]; then
  ESPTOOL=(esptool)   # preview only: do not require esptool for --dry-run
else
  die "esptool not found. Either source your ESP-IDF first:
       source ~/code/esp-idf/export.sh
     or run this script inside the esphome image (see --help)."
fi
# esptool v4 uses write_flash/verify_flash, v5 renamed them to
# write-flash/verify-flash (dashes work in all v5 builds).
ESPTOOL_VER="$("${ESPTOOL[@]}" version 2>/dev/null | head -1 || true)"
if [[ "${ESPTOOL_VER}" == *"v5"* || "${ESPTOOL_VER}" == 5.* ]]; then
  WRITE_CMD=write-flash; VERIFY_CMD=verify-flash
else
  WRITE_CMD=write_flash; VERIFY_CMD=verify_flash
fi

# --- what we are about to do ----------------------------------------------
echo "config        : ${CONFIG_NAME}"
echo "port          : ${PORT}"
echo "model offset  : ${MODEL_OFFSET}   (from partitions.csv)"
echo "using         : ${ESPTOOL[*]}  (${ESPTOOL_VER:-unknown}; ${WRITE_CMD})"
echo
for ((i = 0; i < ${#IMAGES[@]}; i += 2)); do
  f="${IMAGES[$((i + 1))]}"
  printf '  %-10s %-28s %9s bytes  md5 %s\n' \
    "${IMAGES[$i]}" "$(basename "${f}")" "$(stat -c %s "${f}")" "$(md5sum "${f}" | cut -d' ' -f1)"
done
echo

CMD=("${ESPTOOL[@]}" --chip esp32s3 --port "${PORT}" --baud 921600 "${WRITE_CMD}" "${IMAGES[@]}")

if [[ "${DRY_RUN}" == "1" ]]; then
  echo "would run:"
  printf '  %s \\\n' "${CMD[@]}"
  exit 0
fi

"${CMD[@]}"

echo
echo "Verifying the model blob:"
"${ESPTOOL[@]}" --chip esp32s3 --port "${PORT}" "${VERIFY_CMD}" "${MODEL_OFFSET}" "${MODELS}" || true
echo
cat <<EOF
The board now reboots into the new layout. Verify over the API or serial
(the models must come from the ${MODEL_OFFSET} partition):

  AFE: loaded esp-sr models from the 'model' partition (2 models, 2048 KiB)

  esphome logs /config/${CONFIG_NAME}.yaml --device <ip-or-hostname>

Future updates stay OTA and never touch the model partition:

  esphome upload /config/${CONFIG_NAME}.yaml --device <ip-or-hostname>
EOF
