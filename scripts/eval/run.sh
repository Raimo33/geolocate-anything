#!/bin/bash
# End-to-end test on real recordings, the way the dashboard runs an upload (`geoloc locate`).
#
#   scripts/eval/run.sh download            # yt-dlp best audio + info.json for every id in truth.tsv
#   scripts/eval/run.sh MODE ID...          # MODE: dated (±1 day around the true start) | blind (no date)
#                                           #       | forced50 | forced60 (dated, hum frequency forced)
#   scripts/eval/score.py                   # table of every report in $OUT against truth.tsv
#
# Env: GEOLOC (binary, default build/geoloc), EVAL_AUDIO (downloads), OUT (reports), MEM (cap per run,
# default 2500M), YTDLP (a recent yt-dlp; the distro one gets 403s), and the dashboard's GEOLOC_* bucket
# variables (e.g. `railway run -- scripts/eval/run.sh ...`), so references and ADS-B days come from the buckets.
# Every run is a capped systemd scope: the laptop has ~7 GB and an OOM kill takes other work down with it.
set -u
H=$(cd "$(dirname "$0")" && pwd)
GEOLOC=${GEOLOC:-$H/../../build/geoloc}
EVAL_AUDIO=${EVAL_AUDIO:-$HOME/.cache/geolocate-anything/wild/eval-2026-11}
OUT=${OUT:-$EVAL_AUDIO/out}
mkdir -p "$EVAL_AUDIO" "$OUT"

if [ "${1:-}" = download ]; then
  cut -f1 "$H/truth.tsv" | tail -n +2 | grep -v '^colsto' | sed 's#^#https://www.youtube.com/watch?v=#' > "$EVAL_AUDIO/ids.txt"
  cd "$EVAL_AUDIO" && exec ${YTDLP:-yt-dlp} --js-runtimes node -f bestaudio -o "%(id)s.%(ext)s" --write-info-json \
    --no-progress --ignore-errors -N 4 -- $(cat ids.txt)
fi

mode=$1; shift
for id in "$@"; do
  f=$(ls "$EVAL_AUDIO/$id".* 2>/dev/null | grep -v -E '\.json$|\.part$' | head -1)
  [ -z "$f" ] && { echo "$id: no audio in $EVAL_AUDIO"; continue; }
  rel=$(python3 -c "import json,sys;print(int(json.load(open(sys.argv[1]))['release_timestamp']))" "$EVAL_AUDIO/$id.info.json")
  args=()
  case $mode in
    dated|forced50|forced60)
      args=(--from "$(date -u -d @$((rel - 86400)) +%FT%TZ)" --to "$(date -u -d @$((rel + 86400)) +%FT%TZ)");;
  esac
  case $mode in forced50) args+=(--nominal 50);; forced60) args+=(--nominal 60);; esac
  bbox=$(awk -F'\t' -v id="$id" '$1 == id { print $9 }' "$H/truth.tsv")
  [ -n "$bbox" ] && args+=("--aircraft-bbox=$bbox")
  name=$mode-$id
  (cd "$OUT" && /usr/bin/time -v systemd-run --user --scope -q -p MemoryMax="${MEM:-2500M}" -p MemorySwapMax=0 \
    "$GEOLOC" locate "$f" --json "$name.json" "${args[@]}" > "$name.txt" 2> "$name.err")
  echo "$name exit=$? $(grep Elapsed "$OUT/$name.err" | awk '{print $NF}') rss=$(grep 'Maximum resident' "$OUT/$name.err" | awk '{print $NF}')kB"
done
