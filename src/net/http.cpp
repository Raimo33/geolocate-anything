#include "net/http.hpp"

#include <curl/curl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <deque>
#include <stdexcept>
#include <unordered_map>

namespace geo::net {

namespace {

struct CurlGlobal {
  CurlGlobal() { curl_global_init(CURL_GLOBAL_DEFAULT); }
  ~CurlGlobal() { curl_global_cleanup(); }
};

struct Transfer {
  CURL* easy = nullptr;
  Request req;
  Response resp;
  curl_slist* hdrs = nullptr;
  std::FILE* upload = nullptr;
  bool ignored_range = false;
  asio::any_completion_handler<void(Response)> handler;
  ~Transfer() {
    if (hdrs) curl_slist_free_all(hdrs);
    if (upload) std::fclose(upload);
    if (easy) curl_easy_cleanup(easy);
  }
};

std::size_t on_write(char* p, std::size_t sz, std::size_t n, void* user) {
  auto* t = static_cast<Transfer*>(user);
  std::string_view chunk(p, sz * n);
  if (t->req.on_data) {
    long code = 0;
    curl_easy_getinfo(t->easy, CURLINFO_RESPONSE_CODE, &code);
    if (code >= 200 && code < 300) {
      if (t->req.range && code != 206) {  // the server ignored the Range header: this is not the part asked for
        t->ignored_range = true;
        return 0;
      }
      return t->req.on_data(chunk) ? chunk.size() : 0;
    }
    t->resp.body.append(chunk.substr(0, std::min<std::size_t>(chunk.size(), 4096 - std::min<std::size_t>(4096, t->resp.body.size()))));
    return chunk.size();  // error bodies are kept (truncated) for messages
  }
  t->resp.body.append(chunk);
  return chunk.size();
}

std::size_t on_header(char* p, std::size_t sz, std::size_t n, void* user) {
  auto* t = static_cast<Transfer*>(user);
  std::string_view line(p, sz * n);
  if (line.starts_with("HTTP/")) {
    t->resp.headers.clear();  // a new response (redirect or 100-continue)
  } else if (auto c = line.find(':'); c != std::string_view::npos) {
    std::string k(line.substr(0, c));
    for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::string_view v = line.substr(c + 1);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
    while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ')) v.remove_suffix(1);
    t->resp.headers[k] = std::string(v);
  }
  return sz * n;
}

}  // namespace

struct Http::Impl {
  struct Sock {
    std::unique_ptr<asio::posix::stream_descriptor> sd;
    int what = 0;
    bool rd = false, wr = false;
  };

  asio::io_context& io;
  CURLM* multi;
  asio::steady_timer timer;
  std::unordered_map<curl_socket_t, std::shared_ptr<Sock>> socks;
  int running = 0;

  explicit Impl(asio::io_context& io_) : io(io_), multi(curl_multi_init()), timer(io_) {
    static CurlGlobal g;
    curl_multi_setopt(multi, CURLMOPT_SOCKETFUNCTION, &Impl::sock_cb);
    curl_multi_setopt(multi, CURLMOPT_SOCKETDATA, this);
    curl_multi_setopt(multi, CURLMOPT_TIMERFUNCTION, &Impl::timer_cb);
    curl_multi_setopt(multi, CURLMOPT_TIMERDATA, this);
    curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, 8L);
  }
  ~Impl() {
    timer.cancel();
    for (auto& [s, sp] : socks) forget(sp);
    curl_multi_cleanup(multi);
  }

  // Stop watching a socket without closing it (curl owns the fd). release() also
  // cancels pending waits; nothing here may throw: the fd can already be closed.
  static void forget(const std::shared_ptr<Sock>& sp) {
    sp->what = 0;
    boost::system::error_code ec;
    sp->sd->cancel(ec);
    try {
      sp->sd->release();
    } catch (const std::exception&) {
    }
  }
  void drop(curl_socket_t s) {
    if (auto it = socks.find(s); it != socks.end()) {
      forget(it->second);
      socks.erase(it);
    }
  }
  // curl closes sockets through us, so asio forgets an fd before its number can be
  // reused by a new connection.
  static int close_cb(void* userp, curl_socket_t s) {
    static_cast<Impl*>(userp)->drop(s);
    return ::close(s);
  }

  static int sock_cb(CURL*, curl_socket_t s, int what, void* userp, void*) {
    auto* self = static_cast<Impl*>(userp);
    if (what == CURL_POLL_REMOVE) {
      self->drop(s);
      return 0;
    }
    auto& sp = self->socks[s];
    if (!sp) {
      sp = std::make_shared<Sock>();
      sp->sd = std::make_unique<asio::posix::stream_descriptor>(self->io, s);
    }
    sp->what = what;
    self->arm(s, sp);
    return 0;
  }

  void arm(curl_socket_t s, const std::shared_ptr<Sock>& sp) {
    auto waiter = [this, s](std::weak_ptr<Sock> w, bool read) {
      return [this, s, w, read](boost::system::error_code ec) {
        auto p = w.lock();
        if (!p) return;
        (read ? p->rd : p->wr) = false;
        if (ec) return;
        action(s, read ? CURL_CSELECT_IN : CURL_CSELECT_OUT);
        auto it = socks.find(s);
        if (it != socks.end() && it->second == p) arm(s, p);
      };
    };
    if ((sp->what & CURL_POLL_IN) && !sp->rd) {
      sp->rd = true;
      sp->sd->async_wait(asio::posix::stream_descriptor::wait_read, waiter(sp, true));
    }
    if ((sp->what & CURL_POLL_OUT) && !sp->wr) {
      sp->wr = true;
      sp->sd->async_wait(asio::posix::stream_descriptor::wait_write, waiter(sp, false));
    }
  }

  static int timer_cb(CURLM*, long ms, void* userp) {
    auto* self = static_cast<Impl*>(userp);
    self->timer.cancel();
    if (ms >= 0) {
      self->timer.expires_after(std::chrono::milliseconds(ms));
      self->timer.async_wait([self](boost::system::error_code ec) {
        if (!ec) self->action(CURL_SOCKET_TIMEOUT, 0);
      });
    }
    return 0;
  }

  void action(curl_socket_t s, int flags) {
    curl_multi_socket_action(multi, s, flags, &running);
    check_done();
  }

  void check_done() {
    int left = 0;
    while (CURLMsg* m = curl_multi_info_read(multi, &left)) {
      if (m->msg != CURLMSG_DONE) continue;
      Transfer* t = nullptr;
      curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, reinterpret_cast<char**>(&t));
      std::unique_ptr<Transfer> own(t);
      if (m->data.result != CURLE_OK) {
        t->resp.error = curl_easy_strerror(m->data.result);
        if (m->data.result == CURLE_WRITE_ERROR && t->req.on_data) t->resp.error = "aborted by the receiver";
        if (t->ignored_range) t->resp.error = "the server ignored the byte range asked for";
      }
      curl_easy_getinfo(t->easy, CURLINFO_RESPONSE_CODE, &t->resp.status);
      char* eff = nullptr;
      curl_easy_getinfo(t->easy, CURLINFO_EFFECTIVE_URL, &eff);
      if (eff) t->resp.effective_url = eff;
      curl_multi_remove_handle(multi, t->easy);
      auto handler = std::move(t->handler);
      Response r = std::move(t->resp);
      own.reset();
      asio::post(io, [h = std::move(handler), r = std::move(r)]() mutable { std::move(h)(std::move(r)); });
    }
  }

  void start(Request req, asio::any_completion_handler<void(Response)> handler) {
    auto t = std::make_unique<Transfer>();
    t->req = std::move(req);
    t->handler = std::move(handler);
    CURL* e = t->easy = curl_easy_init();
    const auto& r = t->req;
    curl_easy_setopt(e, CURLOPT_URL, r.url.c_str());
    curl_easy_setopt(e, CURLOPT_PRIVATE, t.get());
    curl_easy_setopt(e, CURLOPT_CLOSESOCKETFUNCTION, &Impl::close_cb);
    curl_easy_setopt(e, CURLOPT_CLOSESOCKETDATA, this);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, &on_write);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, t.get());
    curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, &on_header);
    curl_easy_setopt(e, CURLOPT_HEADERDATA, t.get());
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(e, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(e, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, r.connect_timeout_s);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, r.stall_timeout_s);
    if (r.total_timeout_s > 0) curl_easy_setopt(e, CURLOPT_TIMEOUT, r.total_timeout_s);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_TCP_KEEPALIVE, 1L);
    if (r.decompress && !r.range) curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "");
    if (r.range) {
      std::string rg = std::to_string(r.range->first) + "-" + std::to_string(r.range->second);
      curl_easy_setopt(e, CURLOPT_RANGE, rg.c_str());  // curl copies the string
    }
    for (auto& h : r.headers) t->hdrs = curl_slist_append(t->hdrs, h.c_str());
    if (r.method == "HEAD") {
      curl_easy_setopt(e, CURLOPT_NOBODY, 1L);
    } else if (!r.upload_file.empty()) {
      t->upload = std::fopen(r.upload_file.c_str(), "rb");
      if (!t->upload) {
        t->resp.error = "cannot read " + r.upload_file;
        auto h = std::move(t->handler);
        asio::post(io, [h = std::move(h), resp = t->resp]() mutable { std::move(h)(std::move(resp)); });
        return;
      }
      std::fseek(t->upload, 0, SEEK_END);
      long sz = std::ftell(t->upload);
      std::fseek(t->upload, 0, SEEK_SET);
      curl_easy_setopt(e, CURLOPT_UPLOAD, 1L);
      curl_easy_setopt(e, CURLOPT_READDATA, t->upload);
      curl_easy_setopt(e, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(sz));
      t->hdrs = curl_slist_append(t->hdrs, "Expect:");
    } else if (r.method != "GET") {
      curl_easy_setopt(e, CURLOPT_CUSTOMREQUEST, r.method.c_str());
      if (!r.body.empty() || r.method == "POST" || r.method == "PUT") {
        curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(r.body.size()));
        curl_easy_setopt(e, CURLOPT_COPYPOSTFIELDS, r.body.c_str());
      }
    }
    if (!r.aws_sigv4.empty()) {
      curl_easy_setopt(e, CURLOPT_AWS_SIGV4, r.aws_sigv4.c_str());
      curl_easy_setopt(e, CURLOPT_USERPWD, r.userpwd.c_str());
    }
    if (t->hdrs) curl_easy_setopt(e, CURLOPT_HTTPHEADER, t->hdrs);
    curl_multi_add_handle(multi, e);
    t.release();  // owned by curl until CURLMSG_DONE
  }
};

Http::Http(asio::io_context& io) : io_(io), impl_(std::make_unique<Impl>(io)) {}
Http::~Http() = default;

asio::awaitable<Response> Http::fetch(Request req) {
  co_return co_await asio::async_initiate<decltype(asio::use_awaitable), void(Response)>(
      [this](auto handler, Request r) { impl_->start(std::move(r), std::move(handler)); }, asio::use_awaitable, std::move(req));
}

asio::awaitable<Response> Http::get(std::string url, int retries, long total_timeout_s) {
  std::string last;
  for (int attempt = 0; attempt < retries; ++attempt) {
    if (attempt) co_await sleep_for(double(1 << attempt));
    Request rq;
    rq.url = url;
    rq.total_timeout_s = total_timeout_s;
    Response r = co_await fetch(std::move(rq));
    if (r.ok()) co_return r;
    last = r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
    if (r.error.empty() && r.status >= 400 && r.status < 500 && r.status != 429 && r.status != 408) break;  // won't change
  }
  throw std::runtime_error("download failed: " + url + ": " + last);
}

asio::awaitable<void> sleep_for(double seconds) {
  asio::steady_timer t(co_await asio::this_coro::executor);
  t.expires_after(std::chrono::milliseconds(static_cast<long>(seconds * 1000)));
  co_await t.async_wait(asio::use_awaitable);
}

asio::awaitable<void> AsyncEvent::wait() {
  if (set_) co_return;
  boost::system::error_code ec;
  co_await timer_.async_wait(asio::redirect_error(asio::use_awaitable, ec));
}

void AsyncEvent::set() {
  set_ = true;
  timer_.cancel();
}

void AsyncEvent::reset() {
  set_ = false;
  timer_.expires_at(asio::steady_timer::time_point::max());
}

asio::awaitable<void> AsyncSemaphore::acquire() {
  if (free_ > 0) {
    --free_;
    co_return;
  }
  auto ev = std::make_shared<AsyncEvent>(ex_);
  waiters_.push_back(ev);
  co_await ev->wait();  // the releaser hands its slot over
}

void AsyncSemaphore::release() {
  if (!waiters_.empty()) {
    waiters_.front()->set();
    waiters_.pop_front();
  } else {
    ++free_;
  }
}

std::recursive_mutex& event_loop_mutex() {
  static std::recursive_mutex m;
  return m;
}

void run_sync_void(const std::function<asio::awaitable<void>(asio::io_context&)>& f) {
  std::lock_guard lk(event_loop_mutex());
  asio::io_context io;
  std::exception_ptr err;
  asio::co_spawn(io, f(io), [&](std::exception_ptr e) { err = e; });
  io.run();
  if (err) std::rethrow_exception(err);
}

}  // namespace geo::net
