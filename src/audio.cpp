#include "audio.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <numeric>
#include <future>
#include <deque>
#include <cctype>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>

#include "util/parallel.hpp"
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

namespace {

using FramePtr = std::unique_ptr<AVFrame, void (*)(AVFrame*)>;
FramePtr new_frame() {
  return FramePtr(av_frame_alloc(), [](AVFrame* p) { av_frame_free(&p); });
}

// The file's audio stream with a decoder opened on the calling thread.
struct Decoder {
  FormatCtx f;
  CodecCtx c;
  int si = -1;
};
void open_decoder(Decoder& d, const std::string& path) {
  open_input(d.f, path);
  const AVCodec* dec = nullptr;
  d.si = av_find_best_stream(d.f.p, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
  if (d.si < 0 || !dec) throw std::runtime_error("no audio stream decoded from " + path);
  for (unsigned i = 0; i < d.f.p->nb_streams; ++i)
    if (int(i) != d.si) d.f.p->streams[i]->discard = AVDISCARD_ALL;  // don't demux video
  d.c.p = avcodec_alloc_context3(dec);
  avcodec_parameters_to_context(d.c.p, d.f.p->streams[d.si]->codecpar);
  d.c.p->thread_count = 1;  // decode on the calling thread
  int e = avcodec_open2(d.c.p, dec, nullptr);
  if (e < 0) throw std::runtime_error("cannot decode " + path + ": " + av_err(e));
}

// Decodes packets in file order and hands each frame to on_frame until it returns
// false (stop) or the stream ends.
template <class F>
void decode_frames(Decoder& d, F&& on_frame) {
  std::unique_ptr<AVPacket, void (*)(AVPacket*)> pkt(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); });
  auto fr = new_frame();
  bool go = true;
  auto receive = [&] {
    while (go && avcodec_receive_frame(d.c.p, fr.get()) == 0) {
      go = on_frame(fr.get());
      av_frame_unref(fr.get());
    }
  };
  while (go && av_read_frame(d.f.p, pkt.get()) >= 0) {
    if (pkt->stream_index == d.si && avcodec_send_packet(d.c.p, pkt.get()) >= 0) receive();
    av_packet_unref(pkt.get());
  }
  if (!go) return;
  avcodec_send_packet(d.c.p, nullptr);
  receive();
}

struct NotParallel {};  // this file can't be cut into chunks: decode it in one pass

// Compressed audio decodes one frame after another on one core (Opus: ~6 s per hour
// of audio on a fast core). Long files are cut into chunks of the stream's own
// timestamps, decoded on several threads: each seeks PREROLL_S before its chunk so the
// decoder's state has settled, and keeps exactly the frames stamped inside its chunk.
// The frames then go through one resampler in file order, as a single pass would.
constexpr double CHUNK_S = 30, PREROLL_S = 2, MIN_PARALLEL_S = 150;

// Chunk i of `bounds` decoded on d: the frames stamped [bounds[i], bounds[i + 1]).
std::vector<FramePtr> decode_chunk(Decoder& d, std::size_t i, const std::vector<std::int64_t>& bounds) {
  const AVRational tb = d.f.p->streams[d.si]->time_base;
  const std::int64_t lo = bounds[i], hi = bounds[i + 1];  // hi < 0: to the end
  const std::int64_t pre = std::int64_t(PREROLL_S / av_q2d(tb));
  if (i > 0) {
    if (av_seek_frame(d.f.p, d.si, lo - pre, AVSEEK_FLAG_BACKWARD) < 0) throw NotParallel{};
    avcodec_flush_buffers(d.c.p);
  }
  bool first_frame = true;
  std::int64_t last = AV_NOPTS_VALUE;
  std::vector<FramePtr> frames;
  decode_frames(d, [&](AVFrame* fr) {
    const std::int64_t ts = fr->best_effort_timestamp;
    if (ts == AV_NOPTS_VALUE || (last != AV_NOPTS_VALUE && ts <= last)) throw NotParallel{};  // no usable order
    last = ts;
    if (first_frame && i > 0 && ts > lo - pre / 4) throw NotParallel{};  // the seek left too little pre-roll
    first_frame = false;
    if (hi >= 0 && ts >= hi) return false;
    if (ts >= lo || i == 0) frames.push_back(FramePtr(av_frame_clone(fr), [](AVFrame* p) { av_frame_free(&p); }));
    return true;
  });
  return frames;
}

// All chunks on `nt` threads, each with its own decoder (a seek and a flush per chunk
// are cheaper than probing the file again), taking the next chunk as it frees up; the
// calling thread hands finished chunks' frames to `consume` in file order. At most nt + 2 chunks
// are decoded ahead of the one being consumed, so memory stays bounded.
template <class F>
void decode_parallel(const std::string& path, const std::vector<std::int64_t>& bounds, int nt, F&& consume) {
  const std::size_t n = bounds.size() - 1, ahead = std::size_t(nt) + 2;
  std::vector<std::optional<std::vector<FramePtr>>> done(n);
  std::mutex mu;
  std::condition_variable cv;
  std::size_t next = 0, consumed = 0;
  bool stop = false;
  std::exception_ptr err;
  auto work = [&] {
    try {
      Decoder d;
      open_decoder(d, path);
      for (;;) {
        std::size_t i;
        {
          std::unique_lock lk(mu);
          cv.wait(lk, [&] { return stop || next >= n || next < consumed + ahead; });
          if (stop || next >= n) return;
          i = next++;
        }
        auto frames = decode_chunk(d, i, bounds);
        std::lock_guard lk(mu);
        done[i] = std::move(frames);
        cv.notify_all();
      }
    } catch (...) {
      std::lock_guard lk(mu);
      if (!err) err = std::current_exception();
      stop = true;
      cv.notify_all();
    }
  };
  std::vector<std::jthread> pool;
  for (int t = 0; t < nt; ++t) pool.emplace_back(work);
  auto finish = [&] {
    {
      std::lock_guard lk(mu);
      stop = true;
    }
    cv.notify_all();
    pool.clear();  // joins
  };
  for (std::size_t c = 0; c < n; ++c) {
    std::vector<FramePtr> frames;
    {
      std::unique_lock lk(mu);
      cv.wait(lk, [&] { return stop || done[c].has_value(); });
      if (!done[c]) break;  // a worker failed
      frames = std::move(*done[c]);
      done[c].reset();
      consumed = c + 1;
    }
    cv.notify_all();
    try {
      consume(std::move(frames));
    } catch (...) {
      finish();
      throw;
    }
  }
  finish();
  if (err) std::rethrow_exception(err);
}

// Decoded chunks laid end to end: each frame's first sample in the one-pass input.
struct Chunk {
  std::vector<FramePtr> frames;
  std::vector<std::int64_t> pos;
  std::int64_t a = 0, b = 0;  // input samples [a, b)
};

// Resampling is a polyphase filter over the whole input (at 48 kHz -> 1 kHz, ~1500 taps
// per output): as slow as decoding, so it is cut too. Chunk c is resampled by a fresh
// resampler that starts RS_PAD_S before it, on an input sample that falls on the output
// grid (a multiple of in / gcd(in, out)), and runs RS_PAD_S into the next chunk; its
// outputs for the chunk's own input span are those of a single pass. The last chunk
// flushes the resampler, as the single pass does at the end.
constexpr double RS_PAD_S = 1.0;

template <class MakeSwr>
void resample_parallel(const std::string& path, const std::vector<std::int64_t>& bounds, int nt, int in_rate, int out_rate,
                       MakeSwr&& make_swr, std::vector<double>& out) {
  const std::int64_t g = std::gcd(std::int64_t(in_rate), std::int64_t(out_rate)), gi = in_rate / g, go = out_rate / g;
  const std::int64_t pad = std::int64_t(RS_PAD_S * in_rate);
  auto first_out = [&](std::int64_t x) { return (x * go + gi - 1) / gi; };  // first output at or after input sample x
  // Feeds input samples [from, to) of the given chunks to r; returns its output.
  auto feed = [&](SwrContext* r, const std::vector<const Chunk*>& cs, std::int64_t from, std::int64_t to, bool flush) {
    std::vector<float> o, buf;
    auto run = [&](const uint8_t** in, int n) {
      const int max_out = swr_get_out_samples(r, n);
      if (max_out <= 0) return;
      buf.resize(std::size_t(max_out));
      auto* op = reinterpret_cast<uint8_t*>(buf.data());
      const int k = swr_convert(r, &op, max_out, in, n);
      if (k < 0) throw std::runtime_error("cannot resample " + path);
      o.insert(o.end(), buf.begin(), buf.begin() + k);
    };
    for (const Chunk* ch : cs)
      for (std::size_t i = 0; i < ch->frames.size(); ++i) {
        const AVFrame* fr = ch->frames[i].get();
        const std::int64_t p0 = ch->pos[i], p1 = p0 + fr->nb_samples, lo = std::max(p0, from), hi = std::min(p1, to);
        if (hi <= lo) continue;
        const auto fmt = AVSampleFormat(fr->format);
        const int bps = av_get_bytes_per_sample(fmt), nch = fr->ch_layout.nb_channels;
        const bool planar = av_sample_fmt_is_planar(fmt);
        std::vector<const uint8_t*> ptr(std::size_t(planar ? nch : 1));
        for (std::size_t p = 0; p < ptr.size(); ++p) ptr[p] = fr->extended_data[p] + (lo - p0) * bps * (planar ? 1 : nch);
        run(ptr.data(), int(hi - lo));
      }
    if (flush) run(nullptr, 0);
    return o;
  };
  // One chunk's outputs: [first_out(a), first_out(b)); `last`: to the end of the input,
  // through `next` too when given (a final chunk shorter than the pad goes with this one).
  auto resample = [&](const Chunk* prev, const Chunk* cur, const Chunk* next, bool last) {
    const std::int64_t S = std::max<std::int64_t>(0, cur->a - pad) / gi * gi, k0 = S / gi * go;
    if (prev ? S < prev->a : S > 0) throw NotParallel{};  // the pad reaches past the previous chunk
    const std::int64_t E = last ? (next ? next->b : cur->b) : std::min(next->b, cur->b + pad);
    SwrContext* r = make_swr();
    std::vector<float> o;
    try {
      std::vector<const Chunk*> cs;
      for (const Chunk* x : {prev, cur, next})
        if (x) cs.push_back(x);
      o = feed(r, cs, S, E, last);
    } catch (...) {
      swr_free(&r);
      throw;
    }
    swr_free(&r);
    const std::int64_t ka = first_out(cur->a) - k0, kb = last ? std::int64_t(o.size()) : first_out(cur->b) - k0;
    if (ka < 0 || kb < ka || kb > std::int64_t(o.size())) throw NotParallel{};  // too little lookahead
    return std::vector<float>(o.begin() + ka, o.begin() + kb);
  };

  // Chunks arrive in order; chunk c is resampled once c + 2 is in (so c + 1, its
  // lookahead, is not the last), at most nt at a time, and the outputs are appended in
  // order. The file's end rarely falls on the chunk grid: a final chunk shorter than the
  // pad (often empty) is resampled with the one before it.
  std::deque<std::future<std::vector<float>>> pending;
  std::int64_t total = 0;
  std::size_t launched = 0;  // chunks resampled or being resampled
  auto collect = [&](std::size_t keep) {
    while (pending.size() > keep) {
      auto v = pending.front().get();
      pending.pop_front();
      out.insert(out.end(), v.begin(), v.end());
    }
  };
  std::vector<std::shared_ptr<Chunk>> all;  // index -> chunk, dropped once no task needs it
  auto launch = [&](std::size_t c, bool last, bool with_next) {
    auto prev = c ? all[c - 1] : nullptr, cur = all[c], next = !last || with_next ? all[c + 1] : nullptr;
    pending.push_back(std::async(std::launch::async, [=, &resample] { return resample(prev.get(), cur.get(), next.get(), last); }));
    collect(std::size_t(nt));
    if (c >= 1) all[c - 1].reset();
  };
  decode_parallel(path, bounds, nt, [&](std::vector<FramePtr>&& frames) {
    auto ch = std::make_shared<Chunk>();
    ch->a = total;
    for (auto& fr : frames) {
      ch->pos.push_back(total);
      total += fr->nb_samples;
    }
    ch->b = total;
    ch->frames = std::move(frames);
    all.push_back(std::move(ch));
    if (all.size() >= launched + 3) launch(launched++, false, false);
  });
  while (launched < all.size()) {
    const std::size_t rem = all.size() - launched;
    if (rem == 2 && all.back()->b - all.back()->a < pad) {
      launch(launched, true, true);
      launched += 2;
    } else {
      launch(launched, rem == 1, false);
      ++launched;
    }
  }
  collect(0);
}

}  // namespace

Audio load_audio(const std::string& path, int rate, int threads) {
  Decoder d;
  open_decoder(d, path);
  Audio out;
  out.path = path;
  for (unsigned i = 0; i < d.f.p->nb_streams; ++i) {
    auto* cp = d.f.p->streams[i]->codecpar;
    if (cp->codec_type == AVMEDIA_TYPE_AUDIO && cp->sample_rate > 0) {
      out.source_rate = cp->sample_rate;
      break;
    }
  }
  if (rate <= 0) rate = out.source_rate.value_or(DEFAULT_ANALYSIS_RATE);  // native rate
  out.rate = rate;
  AVCodecContext* c = d.c.p;

  auto make_swr = [&] {
    SwrContext* p = nullptr;
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    AVChannelLayout in_layout{};
    if (c->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
      av_channel_layout_default(&in_layout, c->ch_layout.nb_channels);
    else
      av_channel_layout_copy(&in_layout, &c->ch_layout);
    int e = swr_alloc_set_opts2(&p, &mono, AV_SAMPLE_FMT_FLT, rate, &in_layout, c->sample_fmt, c->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&in_layout);
    if (e < 0 || swr_init(p) < 0) {
      swr_free(&p);
      throw std::runtime_error("cannot resample " + path);
    }
    return p;
  };
  Swr swr;
  swr.p = make_swr();

  std::vector<float> buf;
  std::vector<double>& s = out.samples;
  if (d.f.p->duration > 0) s.reserve(static_cast<std::size_t>(double(d.f.p->duration) / AV_TIME_BASE * rate * 1.01) + 1024);
  auto drain = [&](AVFrame* fr) {
    int max_out = swr_get_out_samples(swr.p, fr ? fr->nb_samples : 0);
    if (max_out <= 0) return;
    buf.resize(std::size_t(max_out));
    auto* o = reinterpret_cast<uint8_t*>(buf.data());
    int n = swr_convert(swr.p, &o, max_out, fr ? const_cast<const uint8_t**>(fr->extended_data) : nullptr, fr ? fr->nb_samples : 0);
    for (int i = 0; i < n; ++i) s.push_back(buf[std::size_t(i)]);
  };

  // Chunk boundaries in the stream's time base. PCM decodes faster than it reads, and a
  // stream that can't seek or has no length can't be cut: those take one pass.
  const AVStream* st = d.f.p->streams[d.si];
  const double dur = d.f.p->duration > 0 ? double(d.f.p->duration) / AV_TIME_BASE : 0;
  const AVCodecID id = c->codec_id;
  const bool pcm = id >= AV_CODEC_ID_PCM_S16LE && id < AV_CODEC_ID_ADPCM_IMA_QT;
  const int nt = worker_threads(threads);
  bool parallel = nt > 1 && dur >= MIN_PARALLEL_S && !pcm && d.f.p->pb && (d.f.p->pb->seekable & AVIO_SEEKABLE_NORMAL);
  if (parallel) {
    const double tb = av_q2d(st->time_base);
    const std::int64_t t0 = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    const std::size_t n = std::size_t(std::ceil(dur / CHUNK_S));
    std::vector<std::int64_t> bounds(n + 1);
    for (std::size_t i = 0; i < n; ++i) bounds[i] = t0 + std::int64_t(std::llround(double(i) * CHUNK_S / tb));
    bounds[n] = -1;
    try {
      resample_parallel(path, bounds, nt, c->sample_rate, rate, make_swr, s);
      out.chunked = true;
    } catch (const NotParallel&) {
      parallel = false;  // start over in one pass
      s.clear();
    }
  }
  if (!parallel) {
    decode_frames(d, [&](AVFrame* fr) {
      drain(fr);
      return true;
    });
    drain(nullptr);  // flush the resampler
  }
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
