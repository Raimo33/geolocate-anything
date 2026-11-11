#include "audio.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>

#include "util/text.hpp"

namespace geo::audio {

namespace {

struct FormatCtx {
  AVFormatContext* p = nullptr;
  ~FormatCtx() {
    if (p) avformat_close_input(&p);
  }
};
struct CodecCtx {
  AVCodecContext* p = nullptr;
  ~CodecCtx() { avcodec_free_context(&p); }
};
struct Swr {
  SwrContext* p = nullptr;
  ~Swr() { swr_free(&p); }
};

std::string av_err(int e) {
  char b[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(e, b, sizeof b);
  return b;
}

void open_input(FormatCtx& f, const std::string& path) {
  av_log_set_level(AV_LOG_QUIET);
  if (!std::filesystem::exists(path)) throw std::invalid_argument("No such file: " + path);
  int e = avformat_open_input(&f.p, path.c_str(), nullptr, nullptr);
  if (e < 0) throw std::runtime_error("cannot open " + path + ": " + av_err(e));
  if ((e = avformat_find_stream_info(f.p, nullptr)) < 0) throw std::runtime_error("cannot read " + path + ": " + av_err(e));
}

// XMP packets and Sony's NonRealTimeMeta XML carry capture times with their UTC offset,
// in boxes FFmpeg does not export (XMP_ / uuid / meta in MP4 and MOV, _PMX in WAV, PRIV
// in ID3 ...). Cameras put them near the start or, after a moov at the end, near the
// end, so the first and last few MiB are searched for the XML itself, whatever holds it.
void scan_xml_times(const std::string& path, std::map<std::string, std::string>& tags) {
  constexpr std::size_t SPAN = 8u << 20;
  std::FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) return;
  auto read_at = [&](long off, std::size_t n) {
    std::string b(n, '\0');
    if (std::fseek(fp, off, SEEK_SET) != 0) return std::string();
    b.resize(std::fread(b.data(), 1, n, fp));
    return b;
  };
  std::string buf;
  std::fseek(fp, 0, SEEK_END);
  const long size = std::ftell(fp);
  if (size > 0) {
    buf = read_at(0, std::min<std::size_t>(SPAN, std::size_t(size)));
    if (std::size_t(size) > SPAN) buf += read_at(std::max<long>(long(SPAN), size - long(SPAN)), SPAN);
  }
  std::fclose(fp);
  auto value_after = [&](std::size_t p) -> std::string {  // attribute ="v" / ='v', or element >v<
    while (p < buf.size() && std::isspace(static_cast<unsigned char>(buf[p]))) ++p;
    if (p < buf.size() && buf[p] == '=') {
      ++p;
      while (p < buf.size() && std::isspace(static_cast<unsigned char>(buf[p]))) ++p;
      if (p >= buf.size() || (buf[p] != '"' && buf[p] != '\'')) return {};
      const char q = buf[p++];
      auto e = buf.find(q, p);
      return e == std::string::npos || e - p > 64 ? std::string() : buf.substr(p, e - p);
    }
    if (p < buf.size() && buf[p] == '>') {
      auto e = buf.find('<', ++p);
      return e == std::string::npos || e - p > 64 ? std::string() : buf.substr(p, e - p);
    }
    return {};
  };
  // name in the file -> tag key (lower case, as file_clues stores keys)
  static const std::pair<const char*, const char*> FIELDS[] = {
      {"xmp:CreateDate", "xmp:createdate"},
      {"exif:DateTimeOriginal", "exif:datetimeoriginal"},
      {"photoshop:DateCreated", "photoshop:datecreated"},
      {"xmpDM:shotDate", "xmpdm:shotdate"},
  };
  if (buf.find("<x:xmpmeta") != std::string::npos || buf.find("<rdf:RDF") != std::string::npos)
    for (auto [name, key] : FIELDS)
      for (std::size_t p = buf.find(name); p != std::string::npos && !tags.count(key); p = buf.find(name, p + 1)) {
        if (p > 0 && buf[p - 1] == '/') continue;  // a closing tag
        if (auto v = value_after(p + std::strlen(name)); !v.empty()) tags.emplace(key, v);
      }
  // Sony: <NonRealTimeMeta ...> ... <CreationDate value="2024-05-03T14:22:11+02:00"/>
  if (auto p = buf.find("<NonRealTimeMeta"); p != std::string::npos)
    if (auto q = buf.find("<CreationDate", p); q != std::string::npos && q - p < 65536)
      if (auto a = buf.find("value", q); a != std::string::npos && a - q < 64)
        if (auto v = value_after(a + 5); !v.empty()) tags.emplace("sony:creationdate", v);
}

}  // namespace

Audio load_audio(const std::string& path, int rate) {
  FormatCtx f;
  open_input(f, path);
  Audio out;
  out.path = path;
  for (unsigned i = 0; i < f.p->nb_streams; ++i) {
    auto* cp = f.p->streams[i]->codecpar;
    if (cp->codec_type == AVMEDIA_TYPE_AUDIO && cp->sample_rate > 0) {
      out.source_rate = cp->sample_rate;
      break;
    }
  }
  if (rate <= 0) rate = out.source_rate.value_or(DEFAULT_ANALYSIS_RATE);  // native rate
  out.rate = rate;
  const AVCodec* dec = nullptr;
  int si = av_find_best_stream(f.p, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
  if (si < 0 || !dec) throw std::runtime_error("no audio stream decoded from " + path);
  for (unsigned i = 0; i < f.p->nb_streams; ++i)
    if (int(i) != si) f.p->streams[i]->discard = AVDISCARD_ALL;  // don't demux video
  CodecCtx c;
  c.p = avcodec_alloc_context3(dec);
  avcodec_parameters_to_context(c.p, f.p->streams[si]->codecpar);
  c.p->thread_count = 1;  // decode on the calling thread
  int e = avcodec_open2(c.p, dec, nullptr);
  if (e < 0) throw std::runtime_error("cannot decode " + path + ": " + av_err(e));

  Swr swr;
  AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
  AVChannelLayout in_layout{};
  if (c.p->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
    av_channel_layout_default(&in_layout, c.p->ch_layout.nb_channels);
  else
    av_channel_layout_copy(&in_layout, &c.p->ch_layout);
  e = swr_alloc_set_opts2(&swr.p, &mono, AV_SAMPLE_FMT_FLT, rate, &in_layout, c.p->sample_fmt, c.p->sample_rate, 0, nullptr);
  av_channel_layout_uninit(&in_layout);
  if (e < 0 || swr_init(swr.p) < 0) throw std::runtime_error("cannot resample " + path);

  std::vector<float> buf;
  std::vector<double>& s = out.samples;
  if (f.p->duration > 0) s.reserve(static_cast<std::size_t>(double(f.p->duration) / AV_TIME_BASE * rate * 1.01) + 1024);
  auto drain = [&](AVFrame* fr) {
    int max_out = swr_get_out_samples(swr.p, fr ? fr->nb_samples : 0);
    if (max_out <= 0) return;
    buf.resize(std::size_t(max_out));
    auto* o = reinterpret_cast<uint8_t*>(buf.data());
    int n = swr_convert(swr.p, &o, max_out, fr ? const_cast<const uint8_t**>(fr->extended_data) : nullptr, fr ? fr->nb_samples : 0);
    for (int i = 0; i < n; ++i) s.push_back(buf[std::size_t(i)]);
  };
  std::unique_ptr<AVPacket, void (*)(AVPacket*)> pkt(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); });
  std::unique_ptr<AVFrame, void (*)(AVFrame*)> fr(av_frame_alloc(), [](AVFrame* p) { av_frame_free(&p); });
  auto receive = [&] {
    while (avcodec_receive_frame(c.p, fr.get()) == 0) {
      drain(fr.get());
      av_frame_unref(fr.get());
    }
  };
  while (av_read_frame(f.p, pkt.get()) >= 0) {
    if (pkt->stream_index == si && avcodec_send_packet(c.p, pkt.get()) >= 0) receive();
    av_packet_unref(pkt.get());
  }
  avcodec_send_packet(c.p, nullptr);
  receive();
  drain(nullptr);  // flush the resampler
  if (s.empty()) throw std::runtime_error("no audio stream decoded from " + path);
  return out;
}

void write_wav(const std::string& path, const std::vector<double>& samples, int rate) {
  std::FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) throw std::runtime_error("cannot write " + path);
  const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * 2);
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, fp); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, fp); };
  std::fwrite("RIFF", 1, 4, fp);
  u32(36 + data_bytes);
  std::fwrite("WAVEfmt ", 1, 8, fp);
  u32(16);
  u16(1);
  u16(1);
  u32(static_cast<uint32_t>(rate));
  u32(static_cast<uint32_t>(rate) * 2);
  u16(2);
  u16(16);
  std::fwrite("data", 1, 4, fp);
  u32(data_bytes);
  std::vector<int16_t> pcm(samples.size());
  for (std::size_t i = 0; i < samples.size(); ++i) pcm[i] = static_cast<int16_t>(std::clamp(samples[i], -1.0, 1.0) * 32767.0);
  std::fwrite(pcm.data(), 2, pcm.size(), fp);
  std::fclose(fp);
}

// "+48.8577+002.2950+035.000/" (decimal degrees only).
std::optional<std::pair<double, double>> parse_iso6709(const std::string& s) {
  std::size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  auto num = [&](double& v) {
    if (i >= s.size() || (s[i] != '+' && s[i] != '-')) return false;
    std::size_t st = i++;
    std::size_t d0 = i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    if (i == d0) return false;
    if (i < s.size() && s[i] == '.') {
      std::size_t f0 = ++i;
      while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
      if (i == f0) i = f0 - 1;  // "12." : the dot is not part of the number
    }
    auto r = to_double(std::string_view(s).substr(st, i - st));
    if (!r) return false;
    v = *r;
    return true;
  };
  double lat, lon;
  if (!num(lat) || !num(lon)) return std::nullopt;
  if (!(-90 <= lat && lat <= 90 && -180 <= lon && lon <= 180)) return std::nullopt;
  return std::pair{lat, lon};
}

FileClues file_clues(const std::string& path) {
  FileClues c;
  FormatCtx f;
  try {
    open_input(f, path);
  } catch (const std::runtime_error&) {
    return c;
  }
  auto collect = [&](AVDictionary* d) {
    const AVDictionaryEntry* e = nullptr;
    while ((e = av_dict_iterate(d, e))) c.all_tags.emplace(lower(e->key), e->value);  // first wins
  };
  collect(f.p->metadata);
  for (unsigned i = 0; i < f.p->nb_streams; ++i) collect(f.p->streams[i]->metadata);
  scan_xml_times(path, c.all_tags);
  static const char* LOCATION_KEYS[] = {"com.apple.quicktime.location.iso6709", "location", "location-eng", "gps", "xyz"};
  static const char* TIME_KEYS[] = {"com.apple.quicktime.creationdate", "creation_time", "date", "encoded_date"};
  static const char* DEVICE_KEYS[] = {"com.apple.quicktime.make", "com.apple.quicktime.model", "make", "model",
                                      "com.android.manufacturer", "com.android.model", "encoder", "handler_name"};
  for (const char* k : LOCATION_KEYS) {
    auto it = c.all_tags.find(k);
    if (it != c.all_tags.end())
      if (auto pos = parse_iso6709(it->second)) {
        c.gps = pos;
        c.gps_raw = it->second;
        break;
      }
  }
  for (const char* k : TIME_KEYS)
    if (auto it = c.all_tags.find(k); it != c.all_tags.end()) {
      c.creation_time = it->second;
      break;
    }
  for (const char* k : DEVICE_KEYS)
    if (auto it = c.all_tags.find(k); it != c.all_tags.end()) c.device.emplace_back(k, it->second);
  if (f.p->duration != AV_NOPTS_VALUE && f.p->duration > 0) c.duration_s = double(f.p->duration) / AV_TIME_BASE;
  return c;
}

}  // namespace geo::audio
