# syntax=docker/dockerfile:1
# Dashboard image (geoloc serve). Reference data lives in a bucket (GEOLOC_GRID_BUCKET): the
# container downloads it on start and uploads what its refreshes fetch.
#
# Railway rebuilds every layer from scratch, so the slow parts live in BuildKit cache
# mounts that do survive between builds: the apt archives, the FFmpeg install (keyed by
# version + configure flags) and ccache for the C++ objects. A deploy that touches a few
# .cpp files recompiles only those. Railway wants cache ids as s/<service id>-<path>.
#
# Build stage: the C++ toolchain, the -dev libraries, and a minimal static FFmpeg (only
# the demuxers/decoders geoloc reads audio with, no network, no programs). Built with
# pthreads: geoloc decodes long files in chunks on several threads (one decoder each, each
# single-threaded), and without them FFmpeg's one-time table setup is not thread-safe:
# concurrent Opus decoders corrupted the heap.
FROM ubuntu:26.04 AS build
RUN --mount=type=cache,id=s/6d3bbf92-2dd3-41d1-b65b-3f33ab5232a7-/var/cache/apt,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,id=s/6d3bbf92-2dd3-41d1-b65b-3f33ab5232a7-/var/lib/apt,target=/var/lib/apt,sharing=locked \
    rm -f /etc/apt/apt.conf.d/docker-clean \
    && apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      g++ make cmake ninja-build pkg-config nasm ccache curl xz-utils ca-certificates \
      libboost-dev libcli11-dev libcurl4-openssl-dev libzstd-dev libfftw3-dev libarchive-dev libssl-dev zlib1g-dev
ARG FFMPEG_VERSION=8.0.1
RUN --mount=type=cache,id=s/6d3bbf92-2dd3-41d1-b65b-3f33ab5232a7-/cache/ffmpeg,target=/cache/ffmpeg <<'EOF'
set -eu
FLAGS="--prefix=/opt/ffmpeg --enable-static --disable-shared --enable-pic \
  --disable-programs --disable-doc --disable-network --disable-autodetect --enable-pthreads \
  --disable-avdevice --disable-avfilter --disable-swscale --disable-everything \
  --enable-zlib --enable-swresample --enable-protocol=file \
  --enable-demuxer=mov,matroska,ogg,wav,w64,mp3,aac,flac,aiff,caf,avi,asf,mpegts,mpegps,amr \
  --enable-decoder=aac,aac_latm,opus,vorbis,mp3,mp3float,mp2,mp2float,flac,alac,ac3,eac3,wmav1,wmav2,amrnb,amrwb,adpcm_ima_wav,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s32le,pcm_f32le,pcm_f64le,pcm_u8,pcm_alaw,pcm_mulaw \
  --enable-parser=aac,aac_latm,opus,vorbis,mpegaudio,flac,ac3"
KEY=/cache/ffmpeg/${FFMPEG_VERSION}-$(echo "$FLAGS $(gcc -dumpfullversion)" | sha256sum | cut -c1-16)
if [ ! -d "$KEY" ]; then
  curl -fsSL --retry 5 --retry-all-errors https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz | tar xJ -C /tmp
  cd /tmp/ffmpeg-${FFMPEG_VERSION}
  ./configure $FLAGS
  make -j"$(nproc)" >/dev/null
  make install >/dev/null
  rm -rf /cache/ffmpeg/* && cp -a /opt/ffmpeg "$KEY.tmp" && mv "$KEY.tmp" "$KEY"
  rm -rf /tmp/ffmpeg-${FFMPEG_VERSION}
else
  cp -a "$KEY" /opt/ffmpeg
fi
EOF
COPY CMakeLists.txt /src/
COPY cmake /src/cmake
COPY src /src/src
COPY data /src/data
# CMakeLists.txt runs the compiler through ccache (direct mode, PCH-aware); data.cpp
# includes a hash of the files it #embeds, so cached objects never go stale.
ENV CCACHE_DIR=/cache/ccache CCACHE_MAXSIZE=2G CCACHE_COMPILERCHECK=content
RUN --mount=type=cache,id=s/6d3bbf92-2dd3-41d1-b65b-3f33ab5232a7-/cache/ccache,target=/cache/ccache \
    cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/ffmpeg \
    && cmake --build /build --target geoloc && strip /build/geoloc && ccache -s

# Runtime: the binary (data and dashboard are compiled in) and its shared libraries.
FROM ubuntu:26.04
RUN --mount=type=cache,id=s/6d3bbf92-2dd3-41d1-b65b-3f33ab5232a7-/var/cache/apt-rt,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,id=s/6d3bbf92-2dd3-41d1-b65b-3f33ab5232a7-/var/lib/apt-rt,target=/var/lib/apt,sharing=locked \
    rm -f /etc/apt/apt.conf.d/docker-clean \
    && apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      libcurl4t64 libzstd1 libfftw3-double3 libfftw3-single3 libarchive13t64 zlib1g ca-certificates tzdata
COPY --from=build /build/geoloc /usr/local/bin/geoloc
ENV GEOLOC_CACHE=/data
EXPOSE 8000
CMD ["sh", "-c", "exec geoloc serve --host 0.0.0.0 --port ${PORT:-8000}"]
