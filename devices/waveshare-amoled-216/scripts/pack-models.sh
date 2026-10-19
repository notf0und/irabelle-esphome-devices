#!/usr/bin/env bash
#
# pack-models.sh — build the ESP-SR model blob for the Waveshare AMOLED 2.16.
#
# What it does
#   1. stages the requested model folders out of the installed esp-sr component
#   2. packs them into srmodels.bin (flat layout esp-sr parses at runtime)
#   3. prints the size and the exact flash command / partition offset
#
# Why
#   ESPHome's `esphome` wake word/STT stack (micro_wake_word) keeps its own
#   models inside the app and is unaffected by this. srmodels.bin is only for
#   ESP-SR's own stages used by the AFE:
#     nsnet2           neural noise suppression  -> enables AFE_TYPE_VC too
#     vadnet1_medium   ESP-SR VAD (unused today; micro_wake_word does VAD)
#
# Two ways to get it onto the device
#   A) partition (what partitions.csv is for)  — needs a one-time USB flash:
#        esptool --chip esp32s3 --port /dev/ttyACM0 write_flash \
#            0x0 firmware.factory.bin  <model-offset> srmodels.bin
#   B) embedded blob (--emit-blob)             — no USB, models live in the app:
#        regenerates custom_components/waveshare_duplex_audio/srmodels_blob.cpp
#        referencing srmodels.bin via .incbin, so `esphome compile && esphome
#        upload` is enough. Costs ~model size of app flash.
#
# Usage (inside the esphome container, or on the host where the repo is mounted)
#   bash scripts/pack-models.sh                        # nsnet2 + vadnet1_medium
#   bash scripts/pack-models.sh --models nsnet_model/nsnet2
#   bash scripts/pack-models.sh --list                 # show every available model
#   bash scripts/pack-models.sh --emit-blob            # also generate the .cpp blob
#   bash scripts/pack-models.sh --out /tmp/srmodels.bin
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"          # this device's folder in the repo
# ESPHome config dir: where .esphome/ (build tree, esp-sr models) lives.
# Inside the container that is /config; override with ESPHOME_CONFIG_DIR.
CONFIG_DIR="${ESPHOME_CONFIG_DIR:-/config}"
COMPONENT_DIR="$(cd "${DEVICE_DIR}/../.." && pwd)/components/waveshare_duplex_audio"
PARTITIONS_CSV="${DEVICE_DIR}/partitions.csv"

# nsnet2 is the noise-suppression model the AFE wants; vadnet1_medium is cheap
# and only needed if ESP-SR ever does the VAD instead of micro_wake_word.
DEFAULT_MODELS="nsnet_model/nsnet2 vadnet_model/vadnet1_medium"

MODELS="${DEFAULT_MODELS}"
OUT="${DEVICE_DIR}/srmodels.bin"
EMIT_BLOB=0
LIST=0

die() { echo "error: $*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --models)    MODELS="${2:-}"; shift 2 ;;
    --out)       OUT="${2:-}"; shift 2 ;;
    --emit-blob) EMIT_BLOB=1; shift ;;
    --list)      LIST=1; shift ;;
    -h|--help)   sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)           die "unknown argument: $1 (try --help)" ;;
  esac
done

# ── locate the esp-sr model tree ─────────────────────────────────────────────────────────────
# Prefer the copy the firmware is actually built against (managed_components),
# fall back to the component cache used by the ESP-IDF build service.
find_model_root() {
  local d key best="" best_key=""
  # 1) the esp-sr the build copied into the project (exact version in use)
  d="$(find "${CONFIG_DIR}/.esphome/build" -maxdepth 4 -type d \
        -path "*managed_components/espressif__esp-sr/model" -print 2>/dev/null | head -1)"
  if [[ -n "$d" ]]; then echo "$d"; return; fi
  # 2) newest esp-sr in the component cache
  while IFS= read -r d; do
    key="$(echo "$d" | grep -o 'esp-sr_[0-9][^/]*' | head -1)"
    if [[ -z "$best" || "$key" > "$best_key" ]]; then best="$d"; best_key="$key"; fi
  done < <(find "${CONFIG_DIR}/.esphome/.espressif" -maxdepth 4 -type d \
             -path "*espressif__esp-sr*/model" -print 2>/dev/null)
  [[ -n "$best" ]] || die "no esp-sr model tree found under ${CONFIG_DIR}/.esphome (build the firmware once first)"
  echo "$best"
}

MODEL_ROOT="$(find_model_root)"

if [[ "$LIST" == "1" ]]; then
  echo "esp-sr models available in ${MODEL_ROOT}:"
  for group in nsnet_model vadnet_model wakenet_model multinet_model; do
    [[ -d "${MODEL_ROOT}/${group}" ]] || continue
    echo "  ${group}/"
    for m in "${MODEL_ROOT}/${group}"/*/; do
      [[ -d "$m" ]] || continue
      printf '    %-28s %6s\n' "$(basename "$m")" "$(du -sh "$m" 2>/dev/null | cut -f1)"
    done
  done
  echo
  echo "Only nsnet/vadnet are used by the AFE. wakenet/multinet are only useful if"
  echo "ESP-SR itself does wake word / command recognition (we use micro_wake_word)."
  exit 0
fi

# ── stage the requested models ───────────────────────────────────────────────
STAGE="$(mktemp -d)"
trap 'rm -rf "${STAGE}"' EXIT

staged=0
for m in ${MODELS}; do
  src="${MODEL_ROOT}/${m}"
  [[ -d "$src" ]] || die "model '${m}' not found under ${MODEL_ROOT} (use --list)"
  name="$(basename "$m")"
  cp -r "$src" "${STAGE}/${name}"
  staged=$((staged + 1))
done
[[ "$staged" -gt 0 ]] || die "no models selected"

# ── pack ─────────────────────────────────────────────────────────────────────
python3 "${SCRIPT_DIR}/pack_model.py" -m "${STAGE}" -o srmodels.bin
mkdir -p "$(dirname "${OUT}")"
mv "${STAGE}/srmodels.bin" "${OUT}"

SIZE=$(stat -c %s "${OUT}")
echo
echo "packed ${staged} model(s): $(echo ${MODELS} | tr ' ' ',')"
echo "  ${OUT}  (${SIZE} bytes, $(( (SIZE + 1023) / 1024 )) KiB)"

# ── where would it go? ───────────────────────────────────────────────────────
MODEL_OFFSET=""
if [[ -f "${PARTITIONS_CSV}" ]]; then
  MODEL_OFFSET="$(awk -F, '/^[[:space:]]*model[[:space:]]*,/ {gsub(/[[:space:]]/,"",$4); print $4}' "${PARTITIONS_CSV}" | head -1)"
fi

cat <<EOF

Next step — pick ONE:

A) partition route (partitions.csv, requires a one-time USB flash of the new
   table + this blob; everything after that stays OTA):
     esphome compile /config/waveshare-amoled-216.yaml
     esptool --chip esp32s3 --port /dev/ttyACM0 write_flash \\
         0x0      /config/.esphome/build/waveshare-amoled-216/build/firmware.factory.bin \\
         ${MODEL_OFFSET:-0xD90000} ${OUT}
   (model partition offset from partitions.csv: ${MODEL_OFFSET:-0xD90000})

B) embedded route (no USB, models travel inside the app image):
     bash scripts/pack-models.sh --emit-blob
     esphome compile ... && esphome upload ...
   This writes ${COMPONENT_DIR}/srmodels_blob.cpp, which .incbin's the blob
   above. Delete that file to go back to a model-less build.
EOF

# ── optional: generate the embedded blob source ──────────────────────────────
if [[ "$EMIT_BLOB" == "1" ]]; then
  # The .cpp is compiled inside the esphome container, where CONFIG_DIR is
  # mounted at /config — so translate the path the assembler must open.
  if [[ "${CONFIG_DIR}" == "/config" ]]; then
    BIN_IN_CONTAINER="${OUT}"
  else
    BIN_IN_CONTAINER="/config/$(realpath --relative-to="${CONFIG_DIR}" "${OUT}")"
  fi
  BLOB_CPP="${COMPONENT_DIR}/srmodels_blob.cpp"
  cat > "${BLOB_CPP}" <<EOF
// AUTO-GENERATED by scripts/pack-models.sh --emit-blob — do not edit.
//
// Embeds the packed ESP-SR model blob into the app's .rodata so the firmware can
// load it with srmodel_load() instead of esp_srmodel_init("model"), which needs
// a dedicated flash partition. Delete this file to build without embedded
// models; the component tests the symbols as weak, so its absence is fine.
//
// Source blob: ${BIN_IN_CONTAINER} ($(stat -c %s "${OUT}") bytes)

extern "C" {
__asm__(
    ".section .rodata\\n"
    ".balign 4\\n"
    ".global srmodels_blob\\n"
    "srmodels_blob:\\n"
    ".incbin \\"${BIN_IN_CONTAINER}\\"\\n"
    ".global srmodels_blob_end\\n"
    "srmodels_blob_end:\\n"
    ".previous\\n");
}  // extern "C"
EOF
  echo
  echo "wrote ${BLOB_CPP}"
fi
