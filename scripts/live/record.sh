#!/usr/bin/env bash
# Record the mains hum of a public livestream, one FLAC file per UTC hour.
#
#   scripts/live/record.sh <name> <page-url> [out-dir]
#
# yt-dlp resolves the stream (Bilibili, Douyu, Huya, YouTube, Twitch ...) and pipes it
# to ffmpeg, which drops the video and keeps mono audio at 1000 samples/s: enough for
# the 50/60 Hz harmonics up to 400 Hz, ~9 MB an hour. Files are cut on the UTC hour and
# named by the wall-clock time they open (the stream's own delay, a few seconds, is not
# corrected). When the stream drops or its URL expires the loop resolves it again.
# Each session is logged to <out-dir>/<name>/sessions.log with the bytes downloaded.
#
# Prototype: run it detached so it survives the desktop app, e.g.
#   systemd-run --user --unit=hum-jinan -p MemoryMax=300M scripts/live/record.sh jinan https://live.bilibili.com/11235702
set -u
name=$1 page=$2
out=${3:-field_samples/live}/$name
here=$(cd "$(dirname "$0")/../.." && pwd)
ytdlp=${YTDLP:-$here/.venv/bin/yt-dlp}
mkdir -p "$out"
export TZ=UTC

while :; do
  start=$(date -u +%FT%TZ)
  # bestaudio where the platform offers an audio-only rendition (YouTube, Twitch),
  # else the smallest muxed one. The pipe counts what came over the network.
  "$ytdlp" -q --no-warnings --js-runtimes node -f 'bestaudio/worst[ext=flv]/worst[ext!=fmp4]/worst' -o - "$page" 2>>"$out/ytdlp.err" \
    | tee >(wc -c >"$out/.bytes") \
    | ffmpeg -nostdin -loglevel error -i pipe:0 -vn -ac 1 -ar 1000 -c:a flac \
        -f segment -segment_time 3600 -segment_atclocktime 1 -strftime 1 -reset_timestamps 1 \
        "$out/%Y%m%dT%H%M%SZ.flac" 2>>"$out/ffmpeg.err"
  sleep 1
  echo "$start $(date -u +%FT%TZ) bytes=$(cat "$out/.bytes" 2>/dev/null)" >>"$out/sessions.log"
  sleep 30
done
