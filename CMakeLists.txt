cmake_minimum_required(VERSION 3.25)
project(geolocate_anything VERSION 0.2.0 LANGUAGES CXX)

# One binary, `geoloc`: CLI, web dashboard and downloaders. Single-threaded by design:
# I/O is asynchronous (Boost.Asio event loop + libcurl multi), CPU work runs on the
# loop or, in the server, in child processes. The one exception is the seismic
# trackers, whose chunks run on threads of their own (net/offload.hpp).

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# x86-64-v3 = AVX2/FMA (every x86 server CPU of the last decade).
set(GEOLOC_ARCH "x86-64-v3")

# Local, root-less dev headers (see README "Building"): .deps/sysroot holds unpacked
# -dev packages when the system doesn't have them.
set(GEOLOC_SYSROOT "${CMAKE_SOURCE_DIR}/.deps/sysroot/usr")
if(EXISTS "${GEOLOC_SYSROOT}/include")
  list(PREPEND CMAKE_PREFIX_PATH "${GEOLOC_SYSROOT}")
  include_directories(SYSTEM "${GEOLOC_SYSROOT}/include" "${GEOLOC_SYSROOT}/include/x86_64-linux-gnu")
  link_directories("${GEOLOC_SYSROOT}/lib/x86_64-linux-gnu")
endif()

# ccache when installed (-DGEOLOC_CCACHE=OFF to skip it). Direct mode works with the
# precompiled header given these sloppiness settings (see the ccache manual).
option(GEOLOC_CCACHE "Compile through ccache when it is installed" ON)
find_program(CCACHE_PROGRAM ccache)
if(GEOLOC_CCACHE AND CCACHE_PROGRAM AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
  set(CMAKE_CXX_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env CCACHE_SLOPPINESS=pch_defines,time_macros ${CCACHE_PROGRAM})
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)                      # Nyquist decoding (net/offload.hpp)
find_package(Boost 1.83 REQUIRED CONFIG)           # header-only: Asio, Beast, JSON
find_package(CURL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)                      # MD5 (bucket ETags)
find_library(ZSTD_LIB zstd REQUIRED)
find_library(FFTW_LIB fftw3 REQUIRED)
find_library(FFTWF_LIB fftw3f REQUIRED)
find_library(ARCHIVE_LIB archive REQUIRED)          # 7z (Fingrid)
find_library(AVFORMAT_LIB avformat REQUIRED)
find_library(AVCODEC_LIB avcodec REQUIRED)
find_library(AVUTIL_LIB avutil REQUIRED)
find_library(SWRESAMPLE_LIB swresample REQUIRED)
find_path(AV_INCLUDE_DIR libavformat/avformat.h PATH_SUFFIXES x86_64-linux-gnu REQUIRED)
find_path(ZSTD_INCLUDE_DIR zstd.h REQUIRED)
find_path(FFTW_INCLUDE_DIR fftw3.h REQUIRED)
find_path(ARCHIVE_INCLUDE_DIR archive.h REQUIRED)
find_path(CLI11_INCLUDE_DIR CLI/CLI.hpp REQUIRED)

set(GEOLOC_WARN -Wall -Wextra -Wno-unused-parameter -Wno-c23-extensions -Wno-missing-field-initializers)
# GCC false positive with BOOST_ASIO_SEPARATE_COMPILATION: coroutine frames come from
# Asio's aligned_alloc-backed recycler and go back through free(), which GCC can no
# longer pair once that code is out of line. Clang (CI) still checks this.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  list(APPEND GEOLOC_WARN -Wno-mismatched-new-delete)
endif()
# No -ffast-math: NaN marks missing data everywhere.
set(GEOLOC_OPT -O3 -march=${GEOLOC_ARCH} -fno-math-errno -fno-trapping-math -fcx-limited-range)

add_library(geoloc_core STATIC
  # Slowest first: Ninja starts jobs in this order, so a long one never runs alone at the end.
  src/web/server.cpp
  src/cli11_impl.cpp
  src/enf/match.cpp
  src/refs/sources_europe.cpp
  src/json_impl.cpp
  src/refs/sources.cpp
  src/air/adsb.cpp
  src/locate.cpp
  src/air/search.cpp
  src/air/bladepass.cpp
  src/refs/parsers.cpp
  src/asio_impl.cpp
  src/refs/seismic_fetch.cpp
  src/refs/sources_archives.cpp
  src/net/http.cpp
  src/enf/extract.cpp
  src/util/time.cpp
  src/util/files.cpp
  src/util/json.cpp
  src/util/text.cpp
  src/dsp/fft.cpp
  src/dsp/filters.cpp
  src/store/codec.cpp
  src/store/store.cpp
  src/cuts.cpp
  src/audio.cpp
  src/grids.cpp
  src/tzclue.cpp
  src/air/doppler.cpp
  src/air/spectrogram.cpp
  src/air/events.cpp
  src/air/bucket.cpp
  src/air/terrain.cpp
  src/air/fusion.cpp
  src/air/commands.cpp
  src/net/zip.cpp
  src/net/spawn.cpp
  src/net/s3.cpp
  src/refs/sheets.cpp
  src/refs/aemo_annual.cpp
  src/refs/source_util.cpp
  src/refs/sources_australia.cpp
  src/refs/remote.cpp
  src/refs/mseed.cpp
  src/refs/seismic.cpp
  src/data.cpp
)
target_include_directories(geoloc_core PUBLIC src)
target_include_directories(geoloc_core SYSTEM PUBLIC ${AV_INCLUDE_DIR} ${ZSTD_INCLUDE_DIR} ${FFTW_INCLUDE_DIR} ${ARCHIVE_INCLUDE_DIR}
                           ${CLI11_INCLUDE_DIR})
target_compile_options(geoloc_core PRIVATE ${GEOLOC_WARN} ${GEOLOC_OPT})
target_compile_definitions(geoloc_core PUBLIC
  BOOST_ASIO_NO_DEPRECATED BOOST_ASIO_DISABLE_THREADS BOOST_ASIO_SEPARATE_COMPILATION BOOST_JSON_NO_LIB CLI11_COMPILE BOOST_BEAST_USE_STD_STRING_VIEW
  GEOLOC_VERSION="${PROJECT_VERSION}" GEOLOC_DATA_DIR="${CMAKE_SOURCE_DIR}/data")
target_link_libraries(geoloc_core PUBLIC
  Boost::headers CURL::libcurl ZLIB::ZLIB OpenSSL::Crypto
  ${ZSTD_LIB} ${FFTW_LIB} ${FFTWF_LIB} ${ARCHIVE_LIB}
  ${AVFORMAT_LIB} ${AVCODEC_LIB} ${SWRESAMPLE_LIB} ${AVUTIL_LIB} Threads::Threads m)
# data.cpp #embeds data/*.json and the dashboard. It also includes a generated header
# holding their hashes: Ninja rebuilds it when they change, and ccache (direct mode,
# which doesn't hash #embed files) sees the change too.
set(GEOLOC_EMBEDDED ${CMAKE_SOURCE_DIR}/data/grids.json ${CMAKE_SOURCE_DIR}/data/grid_areas.json
                    ${CMAKE_SOURCE_DIR}/data/tz_areas.json ${CMAKE_SOURCE_DIR}/data/dashboard.html)
set(GEOLOC_DATA_STAMP ${CMAKE_BINARY_DIR}/gen/data_stamp.hpp)
add_custom_command(OUTPUT ${GEOLOC_DATA_STAMP}
  COMMAND ${CMAKE_COMMAND} "-DFILES=${GEOLOC_EMBEDDED}" -DOUT=${GEOLOC_DATA_STAMP} -P ${CMAKE_SOURCE_DIR}/cmake/data_stamp.cmake
  DEPENDS ${GEOLOC_EMBEDDED} ${CMAKE_SOURCE_DIR}/cmake/data_stamp.cmake VERBATIM)
target_sources(geoloc_core PRIVATE ${GEOLOC_DATA_STAMP})
set_property(SOURCE src/data.cpp APPEND PROPERTY INCLUDE_DIRECTORIES ${CMAKE_BINARY_DIR}/gen)
set_property(SOURCE src/data.cpp APPEND PROPERTY OBJECT_DEPENDS ${GEOLOC_EMBEDDED} ${GEOLOC_DATA_STAMP})
set_source_files_properties(src/data.cpp PROPERTIES COMPILE_OPTIONS "-Wno-pedantic")
# Argument parsing runs once per invocation: unoptimised it compiles in a third of the time.
set_source_files_properties(src/cli11_impl.cpp PROPERTIES COMPILE_OPTIONS "-O0")
# The search's hot loops: reassociation lets reductions and prefix sums vectorise (only
# summation order changes; NaN handling is kept, no -ffinite-math-only). Not used
# elsewhere: extraction reproduces numpy's exact arithmetic where results depend on it.
set_source_files_properties(src/dsp/filters.cpp src/enf/match.cpp PROPERTIES COMPILE_OPTIONS "-fassociative-math;-fno-signed-zeros")

# Precompiled header: Asio, JSON and the common std headers are parsed once per target.
# The *_impl.cpp files compile the libraries' sources and data.cpp only #embeds: no PCH.
option(GEOLOC_PCH "Precompile the common headers (src/pch.hpp)" ON)
if(GEOLOC_PCH)
  target_precompile_headers(geoloc_core PRIVATE src/pch.hpp)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")  # needed for ccache to cache the PCH's users
    target_compile_options(geoloc_core PUBLIC -Xclang -fno-pch-timestamp)
  endif()
  set_source_files_properties(src/json_impl.cpp src/asio_impl.cpp src/cli11_impl.cpp src/data.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  # Their extra math flags change predefined macros, which makes GCC reject the PCH.
  set_source_files_properties(src/dsp/filters.cpp src/enf/match.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

add_executable(geoloc src/main.cpp)
target_compile_options(geoloc PRIVATE ${GEOLOC_WARN} ${GEOLOC_OPT})
target_link_libraries(geoloc PRIVATE geoloc_core)
# Exported symbols, so the crash handler's backtrace names functions even when stripped.
target_link_options(geoloc PRIVATE -rdynamic)
if(GEOLOC_PCH)
  target_precompile_headers(geoloc REUSE_FROM geoloc_core)
endif()
install(TARGETS geoloc RUNTIME DESTINATION bin)

# The unit tests, when GoogleTest is installed.
find_package(GTest)
if(GTest_FOUND)
  enable_testing()
  add_executable(geoloc_tests
    tests/test_main.cpp tests/synth.cpp tests/test_store.cpp tests/test_match.cpp tests/test_extract.cpp
    tests/test_cuts.cpp tests/test_grids.cpp tests/test_sources.cpp tests/test_pipeline.cpp
    tests/test_remote.cpp tests/test_web.cpp tests/test_tzclue.cpp tests/test_seismic.cpp tests/test_aircraft.cpp)
  target_compile_options(geoloc_tests PRIVATE ${GEOLOC_WARN} -O2 -march=${GEOLOC_ARCH})
  target_link_libraries(geoloc_tests PRIVATE geoloc_core GTest::gtest)
  target_compile_definitions(geoloc_tests PRIVATE GEOLOC_BIN="$<TARGET_FILE:geoloc>")
  if(GEOLOC_PCH)
    target_precompile_headers(geoloc_tests PRIVATE src/pch.hpp <gtest/gtest.h>)
  endif()
  add_dependencies(geoloc_tests geoloc)
  include(GoogleTest)
  # Listed when ctest runs, not after every link of the test binary.
  gtest_discover_tests(geoloc_tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 60)
endif()
