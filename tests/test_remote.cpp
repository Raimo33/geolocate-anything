// Bucket mirror of the reference store, against a fake S3 server in a child process.
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <openssl/evp.h>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <ctime>
#include <map>
#include <ranges>

#include "helpers.hpp"
#include "refs/remote.hpp"

using namespace geo;
namespace fs = std::filesystem;
namespace asio = boost::asio;
namespace http = boost::beast::http;

static std::string md5_hex(const std::string& s) {
  unsigned char d[EVP_MAX_MD_SIZE];
  unsigned n = 0;
  EVP_Digest(s.data(), s.size(), d, &n, EVP_md5(), nullptr);
  std::string o;
  for (unsigned i = 0; i < n; ++i) o += "0123456789abcdef"[d[i] >> 4], o += "0123456789abcdef"[d[i] & 15];
  return o;
}

static std::string url_decode(std::string_view s) {
  std::string o;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) o += char(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16)), i += 2;
    else o += s[i];
  }
  return o;
}

// Path-style S3 subset: PUT/GET objects and ListObjectsV2. One request per connection.
[[noreturn]] static void fake_s3(asio::ip::tcp::acceptor& acc) {
  std::map<std::string, std::string> objects;
  std::map<std::string, std::time_t> written;
  for (;;) {
    asio::io_context& io = static_cast<asio::io_context&>(acc.get_executor().context());
    asio::ip::tcp::socket sock(io);
    acc.accept(sock);
    boost::beast::flat_buffer buf;
    http::request<http::string_body> req;
    boost::system::error_code ec;
    http::read(sock, buf, req, ec);
    if (ec) continue;
    std::string target(req.target());
    std::string path = target.substr(0, target.find('?')), query = target.find('?') == std::string::npos ? "" : target.substr(target.find('?') + 1);
    http::response<http::string_body> res{http::status::ok, req.version()};
    res.keep_alive(false);
    const std::string bucket = "/refs-test";
    bool signed_ok = req[http::field::authorization].starts_with("AWS4-HMAC-SHA256");
    if (!signed_ok) {
      res.result(http::status::forbidden);
    } else if (req.method() == http::verb::put) {
      objects[url_decode(path.substr(bucket.size() + 1))] = req.body();
      written[url_decode(path.substr(bucket.size() + 1))] = std::time(nullptr);
      res.set(http::field::etag, "\"" + md5_hex(req.body()) + "\"");
    } else if (path == bucket || path == bucket + "/") {
      std::string prefix;
      for (auto kv : std::views::split(query, '&')) {
        std::string_view p(kv.begin(), kv.end());
        if (p.starts_with("prefix=")) prefix = url_decode(p.substr(7));
      }
      std::string xml = "<ListBucketResult><IsTruncated>false</IsTruncated>";
      for (auto& [k, v] : objects)
        if (k.starts_with(prefix)) {
          char lm[32];
          std::strftime(lm, sizeof lm, "%Y-%m-%dT%H:%M:%S.000Z", std::gmtime(&written[k]));
          xml += "<Contents><Key>" + k + "</Key><LastModified>" + lm + "</LastModified><ETag>&quot;" + md5_hex(v) + "&quot;</ETag></Contents>";
        }
      res.body() = xml + "</ListBucketResult>";
    } else {
      auto it = objects.find(url_decode(path.substr(bucket.size() + 1)));
      if (it == objects.end()) res.result(http::status::not_found);
      else res.body() = it->second;
    }
    res.prepare_payload();
    http::write(sock, res, ec);
  }
}

class Bucket : public test::TempStore {
 protected:
  void SetUp() override {
    TempStore::SetUp();
    asio::io_context io;
    asio::ip::tcp::acceptor acc(io, {asio::ip::make_address("127.0.0.1"), 0});
    const int port = acc.local_endpoint().port();
    pid_ = ::fork();
    if (pid_ == 0) fake_s3(acc);
    ::setenv("GEOLOC_BUCKET", "refs-test", 1);
    ::setenv("AWS_ENDPOINT_URL", ("http://127.0.0.1:" + std::to_string(port)).c_str(), 1);
    ::setenv("AWS_ACCESS_KEY_ID", "x", 1);
    ::setenv("AWS_SECRET_ACCESS_KEY", "x", 1);
    ::setenv("AWS_DEFAULT_REGION", "us-east-1", 1);
  }
  void TearDown() override {
    ::kill(pid_, SIGKILL);
    ::waitpid(pid_, nullptr, 0);
    for (auto v : {"GEOLOC_BUCKET", "AWS_ENDPOINT_URL"}) ::unsetenv(v);
    TempStore::TearDown();
  }
  void month(const std::string& grid, const std::string& m, float v) {
    auto [a, b] = month_bounds(m);
    store::save_month(grid, "neso", m, std::vector<float>(std::size_t(b - a), v), {});
  }
  void wipe_local() {
    for (auto& e : fs::recursive_directory_iterator(store::refs_dir()))
      if (e.is_regular_file()) fs::remove(e.path());
    store::invalidate_caches();
  }
  pid_t pid_ = 0;
};

TEST_F(Bucket, PushThenPullRoundtripSendsOnlyChanges) {
  month("great_britain", "2026-09", 0.01f);
  EXPECT_EQ(refs::remote::push(), 2);  // month file + manifest
  EXPECT_EQ(refs::remote::push(), 0);  // unchanged: nothing sent again
  wipe_local();                        // a fresh container: empty local store
  EXPECT_EQ(refs::remote::pull(), 2);
  auto [a, b] = month_bounds("2026-09");
  for (float v : store::read("great_britain", a, b)) ASSERT_NEAR(v, 0.01, 1e-4);
  EXPECT_EQ(refs::remote::pull(), 0);
  month("great_britain", "2026-10", 0.02f);
  EXPECT_EQ(refs::remote::push("great_britain"), 2);  // new month + updated manifest
  EXPECT_EQ(refs::remote::push("nordic"), 0);
}

// Two machines share the bucket: a push from a store holding an older copy of a month
// leaves the bucket's newer one, and each push or pull merges the manifests, so neither
// machine's months drop out of the other's listing.
TEST_F(Bucket, PushKeepsNewerCopiesAndManifestsMerge) {
  month("great_britain", "2026-09", 0.01f);
  EXPECT_EQ(refs::remote::push(), 2);
  const fs::path mine = dir_;
  geo::test::use_cache(dir_ / "other");  // the other machine, with an older copy of 2026-09
  month("great_britain", "2026-09", 0.05f);
  auto old_file = store::refs_dir() / "great_britain" / "neso" / "2026-09.u16z";
  fs::last_write_time(old_file, fs::last_write_time(old_file) - std::chrono::hours(1));
  month("great_britain", "2026-10", 0.02f);
  EXPECT_EQ(refs::remote::push(), 2);  // 2026-10 and the merged manifest; not the older 2026-09
  geo::test::use_cache(mine);
  EXPECT_EQ(refs::remote::pull(), 2);  // 2026-10 and the manifest (merged here)
  auto [a, b] = month_bounds("2026-09");
  EXPECT_NEAR(store::read("great_britain", a, a + 1)[0], 0.01, 1e-4);
  EXPECT_TRUE(store::has_month("great_britain", "neso", "2026-10"));
}

TEST(Remote, ManifestMergeKeepsBothSidesNewestEntryWinning) {
  auto man = [](const char* js) { return parse_json(js).as_object(); };
  auto m = refs::remote::merge_manifests(
      man(R"({"meta":{"a":1},"months":{"2020-01":{"stored_at":"2026-01-01","coverage":0.5},"2020-02":{"stored_at":"2026-01-01"}},
            "unavailable":{"2020-03":{"checked_at":"2026-01-01"}}})"),
      man(R"({"months":{"2020-01":{"stored_at":"2026-02-01","coverage":1.0},"2020-03":{"stored_at":"2026-02-01"}},
            "unavailable":{"2020-04":{"checked_at":"2026-01-01"}}})"));
  EXPECT_EQ(m.at("months").at("2020-01").at("coverage").as_double(), 1.0);  // the bucket's is newer
  EXPECT_TRUE(m.at("months").as_object().contains("2020-02"));
  EXPECT_TRUE(m.at("months").as_object().contains("2020-03"));
  EXPECT_FALSE(m.at("unavailable").as_object().contains("2020-03"));  // it has data now
  EXPECT_TRUE(m.at("unavailable").as_object().contains("2020-04"));
  EXPECT_EQ(m.at("meta").at("a").as_int64(), 1);
}

