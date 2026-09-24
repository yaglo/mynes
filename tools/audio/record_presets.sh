#!/bin/sh
# Record one ROM through a set of presets with the real frontend and extract
# each recording's mono audio, so the audio chain can be heard in every
# console / cable / RF / speaker configuration. Prints the mean level and the
# level below 120 Hz per preset (hum and bass) from ffmpeg's volumedetect.
#
#   tools/audio/record_presets.sh rom outdir [preset ...]
#
# Without presets, records the eleven that cover every audio path: the two
# monitors with measured speaker networks, the RF sets, the Famicom, and the
# arcade monitor.
set -e
if [ $# -lt 2 ]; then
  echo "usage: $0 rom outdir [preset ...]" >&2
  exit 1
fi
ROM=$1; OUT=$2; shift 2
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN="$ROOT/build/bin/mynes_gpu"
if [ ! -x "$BIN" ]; then
  echo "build the GPU frontend first: $BIN not found" >&2
  exit 1
fi
if [ $# -eq 0 ]; then
  set -- sony_pvm_14l2 toshiba_14af43 living_room_1988 jvc_d_series_2000 bedroom_rf_1990 \
    basement_tv famicom_kitchen stass_favourite zenith_system_3 retro_gaming_setup nec_xm29_arcade
fi
cd "$ROOT"
SECONDS_=${RECORD_SECONDS:-25}
AFTER=${RECORD_AFTER_FRAMES:-120}
mkdir -p "$OUT"
for preset in "$@"; do
  MYNES_BATCH_LOCK=0 "$BIN" --preset "$preset" --offscreen 640x480 --record "$OUT/$preset.mov" \
    --record-seconds "$SECONDS_" --record-after "$AFTER" "$ROM" > "$OUT/$preset.log" 2>&1 || echo "FAILED $preset"
  ffmpeg -y -v error -i "$OUT/$preset.mov" -vn -ac 1 "$OUT/$preset.wav"
  printf '%-22s ' "$preset"
  ffmpeg -i "$OUT/$preset.wav" -af volumedetect -f null - 2>&1 | grep -oE 'mean_volume: [-0-9.]+ dB' | tr '\n' ' '
  ffmpeg -i "$OUT/$preset.wav" -af "lowpass=f=120,volumedetect" -f null - 2>&1 \
    | grep -oE 'mean_volume: [-0-9.]+ dB' | sed 's/mean_volume/bass<120Hz/'
done
