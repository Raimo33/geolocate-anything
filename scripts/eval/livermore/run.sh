#!/bin/bash
# Aircraft evaluation on Praveen Pathak's Livermore passages (Zenodo 10.5281/zenodo.23424278, CC BY 4.0):
# 40 smartphone recordings of single propeller-aircraft passes over one recorder, May 2026, with the
# ADS-B track of each.
#
#   scripts/eval/livermore/run.sh download        # the 4 zips + results CSV into $LIV, unpacked, truth.tsv built
#   scripts/eval/livermore/run.sh events TAG      # `aircraft events` on every recording -> $LIV/out-TAG, scored
#   scripts/eval/livermore/run.sh locate TAG      # `aircraft locate`, one flyover, start known to +-15 s, town box
#   scripts/eval/livermore/score.py $LIV/truth.tsv $LIV/out-TAG          # detection table again
#   scripts/eval/livermore/score_locate.py $LIV/truth.tsv $LIV/loc-TAG   # location table again
#
# Env: GEOLOC (binary, default build/geoloc), LIV (data, default ~/.cache/geolocate-anything/wild/livermore),
# MEM (cap per run, default 2000M), and the ADS-B bucket variables for `locate` (e.g. `railway run -- ...`).
set -u
H=$(cd "$(dirname "$0")" && pwd)
GEOLOC=${GEOLOC:-$H/../../../build/geoloc}
LIV=${LIV:-$HOME/.cache/geolocate-anything/wild/livermore}
REC=https://zenodo.org/records/23424278/files
BOX=-121.84,37.65,-121.70,37.73  # Livermore town, as DOPPLERGEIST's run (chosen before the reference was known)
WAV_OFFSET=5.8                   # the WAV starts this long before the first traced point (median, score.py)

case ${1:-} in
download)
  mkdir -p "$LIV/zip" "$LIV/raw"
  for f in final_results_40_cases.csv "Raw data_V_constant_H_constant.zip" "Raw data_V_constant_H_varying.zip" \
           "Raw data_V_varying_H_constant.zip" "Raw data_V_varying_H_varying.zip"; do
    [ -s "$LIV/zip/$f" ] || curl -sSfL -o "$LIV/zip/$f" "$REC/${f// /%20}?download=1" || exit 1
  done
  for z in "$LIV"/zip/*.zip; do
    n=$(basename "$z" .zip); n=${n#Raw data_}
    mkdir -p "$LIV/raw/$n" && unzip -q -o "$z" -d "$LIV/raw/$n"
  done
  python3 "$H/truth.py" "$LIV" > "$LIV/truth.tsv";;
events)
  O=$LIV/out-$2; mkdir -p "$O"
  tail -n +2 "$LIV/truth.tsv" | while IFS=$'\t' read -r case _ wav _; do
    "$GEOLOC" aircraft events "$wav" --json "$O/$case.json" > "$O/$case.txt" 2>&1
  done
  python3 "$H/score.py" "$LIV/truth.tsv" "$O";;
locate)
  O=$LIV/loc-$2; mkdir -p "$O"
  tail -n +2 "$LIV/truth.tsv" | while IFS=$'\t' read -r case _ wav start _; do
    t0=$(python3 -c "import sys,datetime as d;print(d.datetime.fromisoformat(sys.argv[1].replace('Z','+00:00')).timestamp()-$WAV_OFFSET)" "$start")
    a=$(python3 -c "print(int($t0 - 15))"); b=$(python3 -c "print(int($t0 + 15))")
    /usr/bin/time -v systemd-run --user --scope -q -p MemoryMax="${MEM:-2000M}" -p MemorySwapMax=0 \
      "$GEOLOC" aircraft locate "$wav" --from "$a" --to "$b" "--bbox=$BOX" --min-events 1 --json "$O/$case.json" \
      > "$O/$case.txt" 2> "$O/$case.err"
    echo "$case exit=$? $(grep Elapsed "$O/$case.err" | awk '{print $NF}')"
  done
  python3 "$H/score_locate.py" "$LIV/truth.tsv" "$O";;
*) sed -n 2,14p "$0"; exit 1;;
esac
