// Dashboard API against the real `geoloc serve` process.
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>

#include <boost/asio.hpp>

#include "helpers.hpp"
#include "net/http.hpp"
#include "util/json.hpp"
#include "web/server.hpp"

using namespace geo;

static int free_port() {
  boost::asio::io_context io;
  boost::asio::ip::tcp::acceptor a(io, {boost::asio::ip::make_address("127.0.0.1"), 0});
  return a.local_endpoint().port();
}

static net::Response call(const std::string& url, const std::string& method = "GET", const std::string& body = {},
                          std::vector<std::string> headers = {}) {
  return net::run_sync<net::Response>([&](boost::asio::io_context& io) -> boost::asio::awaitable<net::Response> {
    net::Http http(io);
    net::Request r;
    r.url = url;
    r.method = method;
    r.body = body;
    r.headers = headers;
    co_return co_await http.fetch(r);
  });
}

class Server : public test::TempStore {
 protected:
  void SetUp() override {
    TempStore::SetUp();
    port_ = free_port();
    pid_ = ::fork();
    if (pid_ == 0) {
      ::setenv("GEOLOC_ADMIN_TOKEN", "s3cret", 1);
      ::execl(GEOLOC_BIN, GEOLOC_BIN, "serve", "--port", std::to_string(port_).c_str(), nullptr);
      ::_exit(127);
    }
    base_ = "http://127.0.0.1:" + std::to_string(port_);
    for (int i = 0; i < 100; ++i) {  // wait for the port
      if (call(base_ + "/api/refresh").status == 200) break;
      ::usleep(50000);
    }
  }
  void TearDown() override {
    ::kill(pid_, SIGTERM);
    ::waitpid(pid_, nullptr, 0);
    TempStore::TearDown();
  }
  int port_ = 0;
  pid_t pid_ = 0;
  std::string base_;
};

TEST_F(Server, RefreshNeedsTokenWhenOneIsSet) {
  const std::string json = "Content-Type: application/json";
  EXPECT_EQ(call(base_ + "/api/refresh", "POST", R"({"grid": "all"})", {json}).status, 401);
  EXPECT_EQ(call(base_ + "/api/refresh", "POST", R"({"grid": "all"})", {json, "Authorization: Bearer nope"}).status, 401);
  EXPECT_EQ(call(base_ + "/api/refresh", "POST", R"({"grid": "hawaii"})", {json, "Authorization: Bearer s3cret"}).status, 400);
}

TEST_F(Server, ImportAemoNeedsTokenAndReportsBadFiles) {
  const std::string b = "XyZ";
  std::string body = "--" + b + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"Mainland frequency data 2019.csv\"\r\n\r\n" +
                     "Region,Value\nNSW,50\n\r\n--" + b + "--\r\n";
  const std::string type = "Content-Type: multipart/form-data; boundary=" + b;
  EXPECT_EQ(call(base_ + "/api/import-aemo", "POST", body, {type}).status, 401);
  auto r = call(base_ + "/api/import-aemo", "POST", body, {type, "Authorization: Bearer s3cret"});
  ASSERT_EQ(r.status, 202);
  auto job = jstr(parse_json(r.body), "job");
  for (int i = 0; i < 200; ++i) {
    r = call(base_ + "/api/locate/" + job);
    if (r.status != 200 || jstr(parse_json(r.body), "state").empty()) break;
    ::usleep(50000);
  }
  EXPECT_EQ(r.status, 422);  // nothing stored: the report says why
  EXPECT_NE(r.body.find("Mainland frequency data 2019.csv"), std::string::npos);
  EXPECT_NE(r.body.find("no header row"), std::string::npos);
}

TEST_F(Server, ServesPageGridsAndAreas) {
  auto page = call(base_ + "/");
  EXPECT_EQ(page.status, 200);
  EXPECT_NE(page.body.find("<html"), std::string::npos);
  auto g = call(base_ + "/api/grids");
  EXPECT_EQ(g.status, 200);
  EXPECT_NE(g.body.find(R"("id":"continental_europe")"), std::string::npos);
  auto a = call(base_ + "/api/grid-areas");
  EXPECT_EQ(a.status, 200);
  auto etag = a.header("etag");
  ASSERT_FALSE(etag.empty());
  EXPECT_EQ(call(base_ + "/api/grid-areas", "GET", {}, {"If-None-Match: " + etag}).status, 304);
  EXPECT_EQ(call(base_ + "/nope").status, 404);
}

TEST_F(Server, LocateRejectsMissingAndBadFiles) {
  const std::string b = "XyZ";
  std::string body = "--" + b + "\r\nContent-Disposition: form-data; name=\"from\"\r\n\r\n2019-01-01\r\n--" + b + "--\r\n";
  auto r = call(base_ + "/api/locate", "POST", body, {"Content-Type: multipart/form-data; boundary=" + b});
  EXPECT_EQ(r.status, 400);
  body = "--" + b + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"x.txt\"\r\n\r\nnot audio\r\n--" + b + "--\r\n";
  r = call(base_ + "/api/locate", "POST", body, {"Content-Type: multipart/form-data; boundary=" + b});
  EXPECT_EQ(r.status, 202);  // a job: the result is polled
  auto job = jstr(parse_json(r.body), "job");
  ASSERT_FALSE(job.empty());
  for (int i = 0; i < 200; ++i) {
    r = call(base_ + "/api/locate/" + job);
    if (r.status != 200 || jstr(parse_json(r.body), "state").empty()) break;
    ::usleep(50000);
  }
  EXPECT_EQ(r.status, 422);
  EXPECT_NE(r.body.find("error"), std::string::npos);
  EXPECT_EQ(call(base_ + "/api/locate/nope").status, 404);
}

TEST(Multipart, StreamsPartsAcrossChunkBoundaries) {
  std::string body = "preamble\r\n--B\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\nhello\r\n--B\r\n"
                     "Content-Disposition: form-data; name=\"file\"; filename=\"f.wav\"\r\nContent-Type: audio/wav\r\n\r\n"
                     "\r\n--C binary \r\n data\r\n--B--\r\n";
  for (std::size_t step : {1, 2, 3, 7, 1000}) {
    std::map<std::string, std::string> got;
    std::string cur, fn;
    web::Multipart mp("B", {[&](const std::string& n, const std::string& f) {
                              cur = n;
                              if (!f.empty()) fn = f;
                              return true;
                            },
                            [&](std::string_view d) { got[cur] += d; }, [] {}});
    for (std::size_t i = 0; i < body.size(); i += step) mp.feed(std::string_view(body).substr(i, step));
    EXPECT_TRUE(mp.done());
    EXPECT_EQ(got["a"], "hello");
    EXPECT_EQ(got["file"], "\r\n--C binary \r\n data");
    EXPECT_EQ(fn, "f.wav");
  }
}
