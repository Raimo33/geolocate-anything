#include "audio.hpp"
#include "helpers.hpp"
#include "locate.hpp"
#include "synth.hpp"

using namespace geo;
namespace fs = std::filesystem;

TEST(Audio, WavRoundtripResamplesTo1kHz) {
  std::vector<double> x(16000);
  for (std::size_t i = 0; i < x.size(); ++i) x[i] = 0.5 * std::sin(2 * M_PI * 100 * double(i) / 8000);
  auto dir = test::make_temp_dir("wav");
  auto p = (dir / "a.wav").string();
  audio::write_wav(p, x, 8000);
  auto a = audio::load_audio(p);
  EXPECT_EQ(a.rate, 1000);
  EXPECT_NEAR(a.duration(), 2.0, 0.01);
  EXPECT_EQ(a.source_rate, 8000);
  fs::remove_all(dir);
}

// Long compressed files decode in chunks on several threads; the samples are those of
// one pass. (Needs the ffmpeg command to write the compressed file.)
TEST(Audio, ChunkedDecodeEqualsOnePass) {
  if (std::system("ffmpeg -version > /dev/null 2>&1") != 0) GTEST_SKIP() << "no ffmpeg command";
  auto dir = test::make_temp_dir("chunks");
  // 200 s ends inside a chunk; 180 s on the chunk grid, where the final chunk is empty.
  for (int secs : {200, 180}) {
    std::vector<double> x(std::size_t(8000 * secs));
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = 0.3 * std::sin(2 * M_PI * 50.01 * double(i) / 8000) + 0.1 * std::sin(0.37 * double(i));
    audio::write_wav((dir / "a.wav").string(), x, 8000);
    for (const char* ext : {"flac", "ogg"}) {
      const auto out = (dir / (std::string("a.") + ext)).string();
      if (std::system(("ffmpeg -v error -y -i " + (dir / "a.wav").string() + " " + out + " > /dev/null 2>&1").c_str()) != 0) continue;
      auto one = audio::load_audio(out, 1000, 1), many = audio::load_audio(out, 1000, 4);
      EXPECT_FALSE(one.chunked);
      EXPECT_TRUE(many.chunked) << secs << " s " << ext << ": fell back to one pass";
      ASSERT_EQ(one.samples.size(), many.samples.size()) << secs << " s " << ext;
      for (std::size_t i = 0; i < one.samples.size(); ++i)
        ASSERT_NEAR(one.samples[i], many.samples[i], 1e-5) << secs << " s " << ext << " sample " << i;
    }
  }
  fs::remove_all(dir);
}

class EndToEnd : public ::testing::TestWithParam<const char*> {};

TEST_P(EndToEnd, LocatesGridTimeAndClockError) {
  const std::string grid = GetParam();
  const i64 start = test::use_refstore() + 123456;
  auto ref = *store::load_grid(grid);
  auto x = synth::bury(synth::hum_from_reference(ref, start, 240, 50, 8000, {{1, 0.3}, {2, 1.0}, {3, 0.4}, {4, 0.2}}, -35, 7), -20, 7);
  auto dir = test::make_temp_dir("e2e");
  auto wav = (dir / "clip.wav").string();
  audio::write_wav(wav, x, 8000);
  auto rep = locate::analyze(wav);
  EXPECT_EQ(rep.hum.nominal, 50);
  ASSERT_TRUE(rep.verdict && rep.verdict->candidate) << locate::render_text(rep);
  EXPECT_EQ(rep.verdict->grid, grid);
  EXPECT_NEAR(rep.verdict->candidate->start_utc, double(start), 1.0);
  EXPECT_TRUE(match::located(rep.verdict->level)) << match::level_name(rep.verdict->level);
  EXPECT_NEAR(rep.verdict->candidate->offset_hz, 50 * -35e-6, 0.001);  // clock error recovered
  for (auto& r : rep.results) EXPECT_NE(r.grid, "us_eastern");      // 60 Hz grid never considered
  auto j = locate::to_json(rep);
  EXPECT_EQ(jstr(j.at("verdict"), "grid"), grid);
  fs::remove_all(dir);
}

INSTANTIATE_TEST_SUITE_P(Grids, EndToEnd, ::testing::Values("continental_europe", "nordic", "great_britain"));
