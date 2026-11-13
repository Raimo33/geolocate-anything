#include "web/server.hpp"

#include <openssl/crypto.h>
#include <signal.h>
#include <zlib.h>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <deque>
#include <set>
#include <fstream>
#include <iostream>
#include <map>
#include <openssl/sha.h>

#include "data.hpp"
#include "locate.hpp"
#include "net/http.hpp"
#include "net/spawn.hpp"
#include "refs/remote.hpp"
#include "refs/sources.hpp"
#include "store/store.hpp"
#include "util/files.hpp"
#include "util/json.hpp"
#include "util/text.hpp"

namespace geo::web {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

// ---------------------------------------------------------------- multipart

std::string boundary_of(std::string_view ct) {
  auto p = ct.find("boundary=");
  if (p == std::string_view::npos) return {};
  std::string_view b = ct.substr(p + 9);
  if (auto sc = b.find(';'); sc != std::string_view::npos) b = b.substr(0, sc);
  return std::string(trim(b));
}

Multipart::Multipart(std::string_view boundary, Handler h) : delim_(std::string("\r\n--").append(boundary)), buf_("\r\n"), h_(std::move(h)) {}

void Multipart::feed(std::string_view chunk) {
  buf_.append(chunk);
  run();
}

static std::string param(std::string_view header, std::string_view key) {
  // key="value" in a Content-Disposition header
  std::string pat = std::string(key) + "=\"";
  std::size_t p = 0;
  while ((p = header.find(pat, p)) != std::string_view::npos) {
    if (p == 0 || header[p - 1] == ' ' || header[p - 1] == ';') {
      auto a = p + pat.size(), b = header.find('"', a);
      return std::string(header.substr(a, b == std::string_view::npos ? std::string_view::npos : b - a));
    }
    p += pat.size();
  }
  return {};
}

void Multipart::run() {
  for (;;) {
    switch (state_) {
      case State::Preamble: {
        auto p = buf_.find(delim_);
        if (p == std::string::npos) {
          if (buf_.size() >= delim_.size()) buf_.erase(0, buf_.size() - delim_.size() + 1);
          return;
        }
        buf_.erase(0, p + delim_.size());
        state_ = State::AfterDelim;
        break;
      }
      case State::AfterDelim: {
        if (buf_.size() < 2) return;
        if (buf_.compare(0, 2, "--") == 0) {
          state_ = State::Done;
          buf_.clear();
          return;
        }
        if (buf_.compare(0, 2, "\r\n") != 0) throw std::runtime_error("malformed multipart body");
        buf_.erase(0, 2);
        state_ = State::Headers;
        break;
      }
      case State::Headers: {
        auto p = buf_.find("\r\n\r\n");
        if (p == std::string::npos) {
          if (buf_.size() > 16384) throw std::runtime_error("multipart part headers too long");
          return;
        }
        std::string name, filename;
        for (auto line : split(std::string_view(buf_).substr(0, p), '\n')) {
          if (lower(line.substr(0, 20)).starts_with("content-disposition:")) {
            name = param(line, "name");
            filename = param(line, "filename");
          }
        }
        buf_.erase(0, p + 4);
        want_ = h_.begin(name, filename);
        state_ = State::Data;
        break;
      }
      case State::Data: {
        auto p = buf_.find(delim_);
        if (p == std::string::npos) {
          if (buf_.size() >= delim_.size()) {
            std::size_t keep = delim_.size() - 1, emit = buf_.size() - keep;
            if (want_) h_.data(std::string_view(buf_).substr(0, emit));
            buf_.erase(0, emit);
          }
          return;
        }
        if (want_ && p) h_.data(std::string_view(buf_).substr(0, p));
        if (want_) h_.end();
        buf_.erase(0, p + delim_.size());
        state_ = State::AfterDelim;
        break;
      }
      case State::Done: return;
    }
  }
}

// ---------------------------------------------------------------- server

namespace {

std::string gzip(std::string_view in) {
  z_stream zs{};
  deflateInit2(&zs, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
  std::string out(deflateBound(&zs, uLong(in.size())), '\0');
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
  zs.avail_in = uInt(in.size());
  zs.next_out = reinterpret_cast<Bytef*>(out.data());
  zs.avail_out = uInt(out.size());
  deflate(&zs, Z_FINISH);
  out.resize(zs.total_out);
  deflateEnd(&zs);
  return out;
}

struct Asset {
  std::string raw, gz, etag, type;
  static Asset make(std::string_view body, std::string type) {
    Asset a;
    a.raw = std::string(body);
    a.gz = gzip(body);
    unsigned char d[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(body.data()), body.size(), d);
    static const char* hex = "0123456789abcdef";
    a.etag = "\"";
    for (int i = 0; i < 8; ++i) {
      a.etag += hex[d[i] >> 4];
      a.etag += hex[d[i] & 15];
    }
    a.etag += "\"";
    a.type = std::move(type);
    return a;
  }
};

// Brings grids' references up to date, each grid in a child process, up to
// PARALLEL at once. Each grid writes only its own manifests
// and bucket prefix; grids that share a joint download (refs::refresh_lane) run one
// after another, since that download writes all of them.
//
// Each grid's refresh is Queued (exactly while it waits in queue_), Running (its child
// process is live), then Done or Error; the server always sets the final state, whatever
// the child printed. The bucket download at startup has a status of its own, shown for
// every grid that has not been refreshed since.
class Refresher {
 public:
  enum class State { Queued, Running, Done, Error };
  static constexpr int PARALLEL = 3;

  Refresher(asio::io_context& io, bool pull_first) : wake_(io.get_executor()) {
    asio::co_spawn(io, run(pull_first), [](std::exception_ptr e) {
      try {
        if (e) std::rethrow_exception(e);
      } catch (const std::exception& x) {
        std::cerr << "refresher stopped: " << x.what() << std::endl;
      }
    });
  }
  void submit(const std::vector<std::string>& grids) {
    for (auto& g : grids) {
      if (auto it = status_.find(g); it != status_.end() && (it->second.state == State::Queued || it->second.state == State::Running))
        continue;
      status_[g] = {State::Queued, {}};
      queue_.push_back(g);
    }
    wake_.set();
  }
  json::object snapshot() const {
    json::object o;
    if (pull_)
      for (auto& g : pull_grids_) o[g] = to_json(*pull_);
    for (auto& [g, s] : status_) o[g] = to_json(s);  // newer than the pull's
    return o;
  }

 private:
  struct Status {
    State state;
    json::object info;  // detail, step/steps while running, finished
  };
  static const char* name(State s) {
    switch (s) {
      case State::Queued: return "queued";
      case State::Running: return "running";
      case State::Done: return "done";
      case State::Error: break;
    }
    return "error";
  }
  // A child's progress line: {"state": "running" | "done" | "error", ...}.
  static std::optional<State> child_state(std::string_view s) {
    if (s == "running") return State::Running;
    if (s == "done") return State::Done;
    if (s == "error") return State::Error;
    return std::nullopt;
  }
  static json::object to_json(const Status& s) {
    json::object o = s.info;
    o["state"] = name(s.state);
    return o;
  }
  static Status finished(State st, std::string detail) { return {st, {{"detail", std::move(detail)}, {"finished", iso(now_seconds())}}}; }

  asio::awaitable<bool> pull(const std::string& exe) {
    for (auto& g : grids::load_grids().grids) pull_grids_.push_back(g.id);
    auto show = [&](int done, int total) {
      pull_ = Status{State::Running,
                     {{"detail", "downloading references from the bucket (" + std::to_string(done) + "/" + std::to_string(total) + " files)"},
                      {"step", done},
                      {"steps", total ? total : 1}}};
    };
    show(0, 0);
    int files = 0;
    std::string error;
    // (named, not inline: GCC 15 has an internal error on braced lists inside co_await)
    std::vector<std::string> pull_argv{exe, "refs", "pull", "--progress-json"};
    std::function<void(std::string_view)> on_line = [&](std::string_view line) {
      try {
        auto v = parse_json(line);
        if (jget(v, "done")) show(int(jint(v, "done")), int(jint(v, "total")));
        if (jget(v, "files")) files = int(jint(v, "files"));
        if (jget(v, "error")) error = jstr(v, "error");
      } catch (...) {
      }
    };
    net::ChildResult res;
    try {
      res = co_await net::run_child(pull_argv, on_line);
    } catch (const std::exception& e) {
      res.exit_code = -1;
      error = std::string("cannot start the download: ") + e.what();
    }
    store::invalidate_caches();
    std::string bucket = env("GEOLOC_BUCKET");
    if (res.exit_code == 0) {
      std::cout << "bucket " << bucket << ": " << files << " file(s) downloaded" << std::endl;
      pull_ = finished(State::Done, std::to_string(files) + " file(s) downloaded from the bucket");
      co_return true;
    } else {
      if (error.empty()) error = "exit code " + std::to_string(res.exit_code);
      std::cout << "bucket " << bucket << ": download failed: " << error << std::endl;
      pull_ = finished(State::Error, "downloading from the bucket failed: " + error);
    }
    co_return false;
  }

  asio::awaitable<void> refresh(const std::string& exe, const std::string& grid) {
    status_[grid] = {State::Running, {{"detail", "checking the sources"}, {"step", 0}, {"steps", 1}}};
    std::vector<std::string> argv{exe, "refs", "refresh", "--grid", grid, "--progress-json"};
    std::function<void(std::string_view)> on_progress = [&](std::string_view line) {
      try {
        auto v = parse_json(line);
        if (!v.is_object()) return;
        auto st = child_state(jstr(v, "state"));
        if (!st) return;
        json::object info = v.as_object();
        info.erase("state");
        status_[grid] = {*st, std::move(info)};
      } catch (...) {
      }
    };
    net::ChildResult res;
    try {
      res = co_await net::run_child(argv, on_progress);
    } catch (const std::exception& e) {
      status_[grid] = finished(State::Error, std::string("cannot start the refresh: ") + e.what());
      co_return;
    }
    store::invalidate_caches();
    auto& s = status_[grid];
    if (res.exit_code != 0) {
      if (s.state != State::Error) s = finished(State::Error, "refresh process exited with code " + std::to_string(res.exit_code));
    } else if (s.state != State::Done && s.state != State::Error) {
      s = finished(State::Done, "finished");  // the child said nothing conclusive
    }
  }

  asio::awaitable<void> run(bool pull_first) {
    const std::string exe = net::self_exe();
    // A deploy starts from the bucket's copy; then every grid is brought up to date (only
    // months not stored yet are fetched, and the refreshes upload to the bucket). Not
    // after a failed download: the store would be refetched from the sources for hours
    // to rebuild what the bucket holds.
    bool pulled = false;
    if (pull_first) pulled = co_await pull(exe);  // grids submitted meanwhile wait in the queue
    if (pull_first && !pulled) {
      std::cout << "not refreshing on start: the bucket download failed (Refresh still works)" << std::endl;
    } else if (pulled) {
      std::vector<std::string> all;
      for (auto& g : grids::load_grids().grids)
        if (!refs::sources_covering(g.id).empty()) all.push_back(g.id);
      std::cout << "refreshing " << all.size() << " grid(s) after the bucket download" << std::endl;
      submit(all);
    }
    auto ex = co_await asio::this_coro::executor;
    for (int i = 1; i < PARALLEL; ++i)
      asio::co_spawn(ex, worker(exe), [](std::exception_ptr e) {
        try {
          if (e) std::rethrow_exception(e);
        } catch (const std::exception& x) {
          std::cerr << "refresh worker stopped: " << x.what() << std::endl;
        }
      });
    co_await worker(exe);
  }

  // Takes the first queued grid whose lane is free. reset() re-arms the timer, which
  // cancels pending waits, so a worker resets only an event that was set (every waiter
  // has been woken by then); otherwise idle workers would keep waking each other.
  asio::awaitable<void> worker(std::string exe) {
    for (;;) {
      auto it = std::find_if(queue_.begin(), queue_.end(), [&](const std::string& g) { return !busy_.contains(refs::refresh_lane(g)); });
      if (it == queue_.end()) {
        if (wake_.is_set()) wake_.reset();
        co_await wake_.wait();
        continue;
      }
      std::string grid = std::move(*it);
      queue_.erase(it);
      const std::string lane = refs::refresh_lane(grid);
      busy_.insert(lane);
      co_await refresh(exe, grid);
      busy_.erase(lane);
      wake_.set();  // its lane is free again
    }
  }

  net::AsyncEvent wake_;
  std::deque<std::string> queue_;
  std::set<std::string> busy_;  // lanes with a refresh running
  std::map<std::string, Status> status_;
  std::optional<Status> pull_;
  std::vector<std::string> pull_grids_;
};

// An analysis running (or finished) in a child process. Uploads return a job id at
// once and the page polls it, so no HTTP request has to stay open for the whole
// analysis (proxies cut long requests, e.g. after 5 minutes).
//   Queued -> Running -> Done, and Queued/Running -> Cancelling -> Done (409).
struct Job {
  enum class State { Queued, Running, Cancelling, Done };
  State state = State::Queued;
  int status = 200;              // HTTP status of the finished result
  std::string body;              // the result (JSON)
  double started = now_seconds(), finished = 0;
  int pid = 0;                   // the child process while Running/Cancelling, else 0
  static const char* name(State s) {
    switch (s) {
      case State::Queued: return "queued";
      case State::Running: return "running";
      case State::Cancelling: return "cancelling";
      case State::Done: break;
    }
    return "done";
  }
};

struct App {
  asio::io_context& io;
  std::map<std::string, std::shared_ptr<Job>> jobs;
  std::uint64_t next_job = 1;
  Asset page, areas, tz_areas;
  Refresher refresher;
  net::AsyncSemaphore analyses;  // concurrent analysis processes
  std::string token = env("GEOLOC_ADMIN_TOKEN");
  std::string exe = net::self_exe();
  App(asio::io_context& i)
      : io(i), page(Asset::make(data::dashboard_html(), "text/html; charset=utf-8")),
        areas(Asset::make(data::grid_areas_json(), "application/geo+json")),
        tz_areas(Asset::make(data::tz_areas_json(), "application/geo+json")), refresher(i, refs::remote::enabled()),
        analyses(i.get_executor(), 2) {}
};

using Response = http::response<http::string_body>;

// What a response needs from its request.
struct Req {
  unsigned version = 11;
  bool keep_alive = true, gzip = false;
  std::string if_none_match;
  template <class Body>
  static Req of(const http::request<Body>& m) {
    Req r;
    r.version = m.version();
    r.keep_alive = m.keep_alive();
    auto it = m.find(http::field::accept_encoding);
    r.gzip = it != m.end() && std::string_view(it->value()).find("gzip") != std::string_view::npos;
    if (auto e = m.find(http::field::if_none_match); e != m.end()) r.if_none_match = std::string(e->value());
    return r;
  }
};

Response make(const Req& req, http::status st, std::string body, const std::string& type, bool gz_ok = true) {
  Response r{st, req.version};
  r.set(http::field::server, "geolocate-anything");
  r.set(http::field::content_type, type);
  if (gz_ok && body.size() >= 4096 && req.gzip) {
    body = gzip(body);
    r.set(http::field::content_encoding, "gzip");
    r.set(http::field::vary, "Accept-Encoding");
  }
  r.body() = std::move(body);
  r.keep_alive(req.keep_alive);
  r.prepare_payload();
  return r;
}

Response json_resp(const Req& req, http::status st, const json::value& v) {
  return make(req, st, dump(v, {.indent = -1, .ascii = false}), "application/json");
}

Response asset(const Req& req, const Asset& a, bool revalidate) {
  Response r{http::status::ok, req.version};
  r.set(http::field::server, "geolocate-anything");
  r.set(http::field::content_type, a.type);
  if (revalidate) {
    r.set(http::field::cache_control, "no-cache");
    r.set(http::field::etag, a.etag);
    if (req.if_none_match == a.etag) {
      r.result(http::status::not_modified);
      r.keep_alive(req.keep_alive);
      r.prepare_payload();
      return r;
    }
  }
  if (req.gzip) {
    r.body() = a.gz;
    r.set(http::field::content_encoding, "gzip");
    r.set(http::field::vary, "Accept-Encoding");
  } else {
    r.body() = a.raw;
  }
  r.keep_alive(req.keep_alive);
  r.prepare_payload();
  return r;
}

json::object error_obj(const std::string& msg) { return {{"error", msg}}; }

// Finished jobs are kept for 15 minutes for the page to collect.
void prune_jobs(App& app) {
  const double now = now_seconds();
  std::erase_if(app.jobs, [&](auto& kv) { return kv.second->state == Job::State::Done && now - kv.second->finished > 900; });
}

asio::awaitable<void> run_job(App& app, std::shared_ptr<Job> job, std::vector<std::string> argv, std::shared_ptr<TempFile> file) {
  co_await app.analyses.acquire();
  net::ChildResult res;
  if (job->state == Job::State::Queued) {  // else cancelled while waiting for a slot
    job->state = Job::State::Running;
    try {
      std::function<void(std::string_view)> no_lines;  // named: GCC 15 ICEs on braced args in co_await
      std::function<void(int)> on_start = [job](int pid) {
        job->pid = pid;
        if (job->state == Job::State::Cancelling) ::kill(pid, SIGTERM);
      };
      res = co_await net::run_child(argv, no_lines, on_start);
    } catch (const std::exception& e) {
      res.exit_code = -1;
      res.out = dump(error_obj(std::string("cannot start the analysis: ") + e.what()));
    }
  }
  app.analyses.release();
  job->pid = 0;
  if (job->state == Job::State::Cancelling) {
    job->status = 409;
    job->body = dump(error_obj("cancelled"));
  } else if (res.exit_code == 0) {
    job->body = std::move(res.out);
  } else if (res.exit_code == 3 && !res.out.empty()) {
    job->status = 422;
    job->body = std::move(res.out);
  } else {
    job->status = 500;
    job->body = dump(error_obj("analysis failed (exit code " + std::to_string(res.exit_code) + ")"));
  }
  job->state = Job::State::Done;
  job->finished = now_seconds();
}

// GET /api/locate/<id>: progress, or the result once done. DELETE cancels.
Response handle_job(App& app, const Req& req, http::verb method, const std::string& id) {
  prune_jobs(app);
  auto it = app.jobs.find(id);
  if (it == app.jobs.end())
    return json_resp(req, http::status::not_found, error_obj("unknown analysis (it expired, or the server restarted): upload the file again"));
  auto job = it->second;
  if (method == http::verb::delete_) {
    if (job->state == Job::State::Queued || job->state == Job::State::Running) {
      job->state = Job::State::Cancelling;
      if (job->pid > 0) ::kill(job->pid, SIGTERM);
    }
    return json_resp(req, http::status::ok, json::object{{"job", id}, {"state", Job::name(job->state)}});
  }
  if (job->state != Job::State::Done)
    return json_resp(req, http::status::ok,
                     json::object{{"job", id}, {"state", Job::name(job->state)}, {"elapsed_s", std::nearbyint(now_seconds() - job->started)}});
  return make(req, http::status(job->status), job->body, "application/json");
}

// A multipart upload: its "file" part streamed to a temporary file, other fields kept.
struct Upload {
  std::optional<TempFile> file;
  std::string filename;
  std::map<std::string, std::string> fields;
  std::uint64_t size = 0;
};

asio::awaitable<Upload> receive_upload(beast::tcp_stream& stream, beast::flat_buffer& buf, http::request_parser<http::empty_body>& hp) {
  std::string boundary = boundary_of(std::string(hp.get()[http::field::content_type]));
  // Thrown errors answer 400 and close the connection (the body is left unread).
  if (boundary.empty()) throw std::runtime_error("expected multipart/form-data");
  http::request_parser<http::buffer_body> bp{std::move(hp)};
  bp.body_limit(std::uint64_t(1) << 31);
  std::optional<TempFile> file;
  std::ofstream out;
  std::string filename, field, cur;
  std::map<std::string, std::string> fields;
  std::uint64_t size = 0;
  Multipart mp(boundary, {[&](const std::string& name, const std::string& fn) {
                            cur = name;
                            field.clear();
                            if (name == "file" && !fn.empty()) {
                              filename = fn;
                              auto dot = fn.rfind('.');
                              std::string suffix = dot == std::string::npos ? "" : fn.substr(dot, 10);
                              for (auto& ch : suffix)
                                if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '.') ch = '_';
                              file.emplace(suffix);
                              out.open(file->path, std::ios::binary);
                            }
                            return true;
                          },
                          [&](std::string_view d) {
                            if (cur == "file" && out.is_open()) {
                              out.write(d.data(), std::streamsize(d.size()));
                              size += d.size();
                            } else if (field.size() < 4096) {
                              field.append(d.substr(0, 4096 - field.size()));
                            }
                          },
                          [&] {
                            if (cur != "file") fields[cur] = field;
                          }});
  char chunk[1 << 16];
  while (!bp.is_done()) {
    bp.get().body().data = chunk;
    bp.get().body().size = sizeof chunk;
    stream.expires_after(std::chrono::minutes(5));
    boost::system::error_code ec;
    co_await http::async_read(stream, buf, bp, asio::redirect_error(asio::use_awaitable, ec));
    if (ec == http::error::need_buffer) ec = {};
    if (ec) throw boost::system::system_error(ec);
    mp.feed(std::string_view(chunk, sizeof chunk - bp.get().body().size));
  }
  out.close();
  if (!file || filename.empty()) throw std::runtime_error("no file uploaded");
  Upload up;  // (not braced: GCC 15 has an internal error on braced lists near co_await/co_return)
  up.file.emplace(std::move(*file));
  up.filename = std::move(filename);
  up.fields = std::move(fields);
  up.size = size;
  co_return up;
}

// Starts argv as a job the page polls (GET /api/locate/<id>); the upload's temporary
// file belongs to the job.
Response start_job(App& app, const Req& req, std::vector<std::string> argv, TempFile file) {
  prune_jobs(app);
  auto job = std::make_shared<Job>();
  std::string id = std::to_string(app.next_job++) + "-" + std::to_string(std::uint64_t(now_seconds() * 1000) % 1000000);
  app.jobs[id] = job;
  auto tmp = std::make_shared<TempFile>(std::move(file));
  asio::co_spawn(app.io, run_job(app, job, std::move(argv), tmp), asio::detached);
  return json_resp(req, http::status::accepted, json::object{{"job", id}, {"state", Job::name(job->state)}});
}

bool admin_ok(const App& app, std::string_view authorization) {
  if (app.token.empty()) return true;
  std::string want = "Bearer " + app.token;
  return authorization.size() == want.size() && CRYPTO_memcmp(authorization.data(), want.data(), want.size()) == 0;
}

// POST /api/locate: stream the multipart body to a temporary file and start the
// analysis in a child process (a job the page polls).
asio::awaitable<Response> handle_locate(App& app, beast::tcp_stream& stream, beast::flat_buffer& buf,
                                        http::request_parser<http::empty_body>& hp) {
  const Req req = Req::of(hp.get());
  auto up = co_await receive_upload(stream, buf, hp);
  auto& fields = up.fields;
  std::vector<std::string> argv{app.exe, "locate", up.file->path.string(), "--web-json", "--display-name", up.filename,
                                "--size", std::to_string(up.size)};
  for (auto [key, flag] : {std::pair{"from", "--from"}, std::pair{"to", "--to"}})
    if (auto it = fields.find(key); it != fields.end() && !it->second.empty()) argv.insert(argv.end(), {flag, it->second});
  if (auto it = fields.find("nominal"); it != fields.end() && (it->second == "50" || it->second == "60"))
    argv.insert(argv.end(), {"--nominal", it->second});
  // Aircraft search area (west,south,east,north) for recordings without a hum match.
  if (auto it = fields.find("aircraft_bbox"); it != fields.end() && !it->second.empty()) {
    double b[4];
    char tail;
    if (std::sscanf(it->second.c_str(), "%lf,%lf,%lf,%lf%c", &b[0], &b[1], &b[2], &b[3], &tail) == 4)
      argv.insert(argv.end(), {"--aircraft-bbox", it->second});
  }
  co_return start_job(app, req, std::move(argv), std::move(*up.file));
}

// POST /api/import-aemo (admin): one of AEMO's yearly frequency workbooks, downloaded by
// hand (aemo.com.au refuses automated requests), imported by `refs import-aemo` in a
// child process and, with a bucket, uploaded to it. The job's result is the JSON report.
asio::awaitable<Response> handle_import_aemo(App& app, beast::tcp_stream& stream, beast::flat_buffer& buf,
                                             http::request_parser<http::empty_body>& hp) {
  const Req req = Req::of(hp.get());
  if (!admin_ok(app, hp.get()[http::field::authorization])) {
    auto r = json_resp(req, http::status::unauthorized, error_obj("importing needs the admin token"));
    r.keep_alive(false);  // the body is left unread: the connection cannot carry another request
    co_return r;
  }
  auto up = co_await receive_upload(stream, buf, hp);
  std::vector<std::string> argv{app.exe, "refs", "import-aemo", up.file->path.string(), "--name", up.filename, "--json"};
  if (auto it = up.fields.find("grid"); it != up.fields.end() && (it->second == "australia_nem" || it->second == "australia_tas"))
    argv.insert(argv.end(), {"--grid", it->second});
  co_return start_job(app, req, std::move(argv), std::move(*up.file));
}

asio::awaitable<Response> handle_refresh(App& app, const http::request<http::string_body>& m) {
  const Req req = Req::of(m);
  if (m.method() == http::verb::post) {
    if (!admin_ok(app, m[http::field::authorization]))
      co_return json_resp(req, http::status::unauthorized, error_obj("refreshing needs the admin token"));
    std::string target;
    try {
      target = jstr(parse_json(m.body()), "grid");
    } catch (...) {
    }
    std::vector<std::string> known;
    for (auto& g : grids::load_grids().grids)
      if (!refs::sources_covering(g.id).empty()) known.push_back(g.id);
    if (target == "all") {
      app.refresher.submit(known);
    } else if (std::find(known.begin(), known.end(), target) != known.end()) {
      app.refresher.submit({target});
    } else {
      co_return json_resp(req, http::status::bad_request, error_obj("no downloadable source for grid '" + target + "'"));
    }
  }
  co_return json_resp(req, http::status::ok, app.refresher.snapshot());
}

asio::awaitable<void> session(App& app, tcp::socket sock) {
  beast::tcp_stream stream(std::move(sock));
  beast::flat_buffer buf;
  try {
    for (;;) {
      http::request_parser<http::empty_body> hp;
      hp.header_limit(64 * 1024);
      hp.body_limit(std::uint64_t(1) << 31);
      stream.expires_after(std::chrono::seconds(120));
      co_await http::async_read_header(stream, buf, hp, asio::use_awaitable);
      const auto& h = hp.get();
      std::string target(h.target());
      if (auto q = target.find('?'); q != std::string::npos) target.resize(q);
      Response res;
      bool keep = h.keep_alive();
      if ((target == "/api/locate" || target == "/api/import-aemo") && h.method() == http::verb::post) {
        const Req req = Req::of(h);  // the handlers move the parser away
        try {
          // Not `?:`: GCC 15 miscompiles co_await in both arms of a conditional (the
          // locate handler ran first and moved the parser away, so imports saw no token).
          if (target == "/api/locate") res = co_await handle_locate(app, stream, buf, hp);
          else res = co_await handle_import_aemo(app, stream, buf, hp);
        } catch (const std::runtime_error& e) {
          res = json_resp(req, http::status::bad_request, error_obj(e.what()));
          keep = false;
        }
      } else {
        http::request_parser<http::string_body> sp{std::move(hp)};
        sp.body_limit(1 << 20);
        co_await http::async_read(stream, buf, sp, asio::use_awaitable);
        const auto& m = sp.get();
        const Req req = Req::of(m);
        if (target == "/" && m.method() == http::verb::get) {
          res = asset(req, app.page, false);
        } else if (target == "/api/grid-areas") {
          res = asset(req, app.areas, true);
        } else if (target == "/api/tz-areas") {
          res = asset(req, app.tz_areas, true);
        } else if (target == "/api/grids") {
          res = json_resp(req, http::status::ok, locate::grid_table());
        } else if (target.starts_with("/api/locate/") && (m.method() == http::verb::get || m.method() == http::verb::delete_)) {
          res = handle_job(app, req, m.method(), target.substr(12));
        } else if (target == "/api/refresh" && (m.method() == http::verb::get || m.method() == http::verb::post)) {
          res = co_await handle_refresh(app, m);
        } else {
          res = make(req, http::status::not_found, "Not Found", "text/plain");
        }
      }
      res.keep_alive(keep && res.keep_alive());
      stream.expires_after(std::chrono::seconds(120));
      co_await http::async_write(stream, res, asio::use_awaitable);
      if (!res.keep_alive()) break;
    }
  } catch (const boost::system::system_error&) {
    // client went away / timed out
  } catch (const std::exception& e) {
    std::cerr << "request failed: " << e.what() << std::endl;
  }
  boost::system::error_code ec;
  stream.socket().shutdown(tcp::socket::shutdown_send, ec);
}

asio::awaitable<void> listen(App& app, tcp::endpoint ep) {
  tcp::acceptor acc(app.io, ep);
  for (;;) {
    auto sock = co_await acc.async_accept(asio::use_awaitable);
    asio::co_spawn(app.io, session(app, std::move(sock)), asio::detached);
  }
}

}  // namespace

void serve(const std::string& host, int port) {
  asio::io_context io;
  App app(io);
  auto addr = asio::ip::make_address(host == "localhost" ? "127.0.0.1" : host);
  // The port opens at once; with a bucket the refresher pulls it in a child process
  // meanwhile, so the platform's health check never waits on the download.
  asio::co_spawn(io, listen(app, {addr, static_cast<unsigned short>(port)}), [](std::exception_ptr e) {
    if (e) std::rethrow_exception(e);
  });
  std::cout << "geolocate-anything dashboard on http://" << host << ":" << port << std::endl;
  asio::signal_set stop(io, SIGINT, SIGTERM);
  stop.async_wait([&](auto, int) { io.stop(); });
  io.run();
}

}  // namespace geo::web
