// Audio decoding and container metadata, in-process through the FFmpeg libraries
// (any format FFmpeg reads: wav, mp3, m4a, mp4, mov, ogg, opus, webm ...). Audio is
// decoded and resampled to mono 1 kHz as it streams, so memory stays O(output).
#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace geo::audio {

inline constexpr int DEFAULT_ANALYSIS_RATE = 1000;

struct Audio {
  std::vector<double> samples;  // mono, roughly [-1, 1]
  int rate = DEFAULT_ANALYSIS_RATE;
  std::optional<int> source_rate;
  std::string path;
  bool chunked = false;  // decoded in chunks on several threads (else in one pass)
  double duration() const { return double(samples.size()) / rate; }
};

// Throws std::runtime_error (decode problems) or std::invalid_argument (missing file).
// rate <= 0 keeps the file's own sample rate. Long compressed files are decoded in
// chunks on `threads` threads (0: GEOLOC_THREADS or every core; 1: one pass).
Audio load_audio(const std::string& path, int rate = DEFAULT_ANALYSIS_RATE, int threads = 0);
void write_wav(const std::string& path, const std::vector<double>& samples, int rate);

struct FileClues {
  std::optional<std::pair<double, double>> gps;
  std::optional<std::string> gps_raw, creation_time;
  std::vector<std::pair<std::string, std::string>> device;  // in DEVICE_KEYS order
  std::optional<double> duration_s;
  std::map<std::string, std::string> all_tags;
};
FileClues file_clues(const std::string& path);
std::optional<std::pair<double, double>> parse_iso6709(const std::string& s);

}  // namespace geo::audio
