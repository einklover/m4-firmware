#include "apps/providers/M4NativeProviderHttp.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <algorithm>
#include <memory>

namespace M4NativeProviderHttp {
namespace {

extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

class SinkStream final : public Stream {
 public:
  SinkStream(M4xJsonStream::Sink& sink, size_t maxBytes, ProgressFn progress, CancelFn cancelled)
      : sink_(sink), maxBytes_(maxBytes), progress_(std::move(progress)), cancelled_(std::move(cancelled)) {}

  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* data, size_t len) override {
    if (failed_ || !data || len == 0) return failed_ ? 0 : len;
    if (cancelled_ && cancelled_()) {
      failed_ = true;
      error_ = "cancelled";
      return 0;
    }
    if (bytes_ > maxBytes_ || len > maxBytes_ - bytes_) {
      failed_ = true;
      error_ = "response_too_large";
      return 0;
    }
    if (!sink_.write(data, len)) {
      failed_ = true;
      error_ = "sink_failed";
      return 0;
    }
    bytes_ += len;
    if (progress_) progress_(bytes_);
    return len;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

  bool failed() const { return failed_; }
  const std::string& error() const { return error_; }
  size_t bytes() const { return bytes_; }

 private:
  M4xJsonStream::Sink& sink_;
  size_t maxBytes_ = 0;
  ProgressFn progress_;
  CancelFn cancelled_;
  size_t bytes_ = 0;
  bool failed_ = false;
  std::string error_;
};

class StringSink final : public M4xJsonStream::Sink {
 public:
  explicit StringSink(size_t cap) : cap_(cap) { body_.reserve(std::min<size_t>(cap, 4096)); }
  bool write(const uint8_t* data, size_t len) override {
    if (!data || len == 0) return true;
    if (body_.size() > cap_ || len > cap_ - body_.size()) return false;
    body_.append(reinterpret_cast<const char*>(data), len);
    return true;
  }
  std::string take() { return std::move(body_); }
 private:
  size_t cap_;
  std::string body_;
};

void applyHeaders(HTTPClient& http, const std::vector<Header>& headers) {
  bool hasUa = false;
  bool hasAe = false;
  for (const auto& h : headers) {
    if (h.name.empty()) continue;
    http.addHeader(h.name.c_str(), h.value.c_str());
    std::string low = h.name;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if (low == "user-agent") hasUa = true;
    if (low == "accept-encoding") hasAe = true;
  }
  if (!hasUa) http.addHeader("User-Agent", "Murphy-M4/NativeProvider/1");
  // HTTPClient's transfer decoder handles chunked framing, not gzip payloads.
  if (!hasAe) http.addHeader("Accept-Encoding", "identity");
}

Result perform(const Request& req, M4xJsonStream::Sink& sink,
               const ProgressFn& progress, const CancelFn& cancelled) {
  Result out;
  if (req.url.empty() || req.maxBytes == 0) {
    out.error = "bad_request";
    return out;
  }
  if (WiFi.status() != WL_CONNECTED) {
    out.error = "wifi_not_connected";
    return out;
  }
  if (cancelled && cancelled()) {
    out.error = "cancelled";
    return out;
  }

  HTTPClient http;
  http.setReuse(false);  // single-flight jobs free TLS/internal heap immediately
  http.setTimeout(static_cast<int>(std::max<uint32_t>(1000, req.timeoutMs)));
  http.setFollowRedirects(req.followRedirects ? HTTPC_FORCE_FOLLOW_REDIRECTS
                                               : HTTPC_DISABLE_FOLLOW_REDIRECTS);
  static const char* kCollect[] = {"Set-Cookie", "Content-Type", "Location"};
  http.collectHeaders(kCollect, 3);

  std::unique_ptr<WiFiClientSecure> tls;
  std::unique_ptr<WiFiClient> plain;
  bool begun = false;
  if (req.url.compare(0, 8, "https://") == 0) {
    tls = std::make_unique<WiFiClientSecure>();
    if (req.insecureTls) {
      tls->setInsecure();
    } else {
      const size_t bundleBytes = static_cast<size_t>(x509_crt_bundle_end - x509_crt_bundle_start);
      if (bundleBytes == 0) {
        out.error = "ca_bundle_missing";
        return out;
      }
      tls->setCACertBundle(x509_crt_bundle_start, bundleBytes);
    }
    tls->setHandshakeTimeout(30);
    begun = http.begin(*tls, req.url.c_str());
  } else if (req.url.compare(0, 7, "http://") == 0) {
    plain = std::make_unique<WiFiClient>();
    begun = http.begin(*plain, req.url.c_str());
  } else {
    out.error = "bad_scheme";
    return out;
  }
  if (!begun) {
    out.error = "http_begin_failed";
    return out;
  }

  applyHeaders(http, req.headers);
  int code = 0;
  if (req.method == "GET") {
    code = http.GET();
  } else {
    const uint8_t* body = reinterpret_cast<const uint8_t*>(req.body.data());
    code = http.sendRequest(req.method.c_str(), const_cast<uint8_t*>(body), req.body.size());
  }
  out.status = code;
  if (code <= 0) {
    out.error = "http_request_failed";
    http.end();
    return out;
  }
  if (code < 200 || code >= 300) {
    out.error = std::string("http_") + std::to_string(code);
    http.end();
    return out;
  }
  const int contentLen = http.getSize();
  if (contentLen > 0 && static_cast<size_t>(contentLen) > req.maxBytes) {
    out.error = "response_too_large";
    http.end();
    return out;
  }

  SinkStream stream(sink, req.maxBytes, progress, cancelled);
  const int written = http.writeToStream(&stream);
  out.bytes = stream.bytes();
  if (stream.failed()) {
    out.error = stream.error();
    http.end();
    return out;
  }
  if (written < 0) {
    out.error = "body_stream_failed";
    http.end();
    return out;
  }

  for (const char* name : kCollect) {
    const String v = http.header(name);
    if (v.length()) out.responseHeaders.push_back({name, v.c_str()});
  }
  http.end();
  tls.reset();
  plain.reset();
  out.ok = true;
  return out;
}

}  // namespace

Result requestToSink(const Request& req, M4xJsonStream::Sink& sink,
                     const ProgressFn& progress, const CancelFn& cancelled) {
  return perform(req, sink, progress, cancelled);
}

bool requestSmall(const Request& req, std::string& bodyOut, Result& resultOut,
                  size_t hardCap, const CancelFn& cancelled) {
  bodyOut.clear();
  Request bounded = req;
  bounded.maxBytes = std::min(req.maxBytes, hardCap);
  StringSink sink(bounded.maxBytes);
  resultOut = perform(bounded, sink, {}, cancelled);
  if (!resultOut.ok) return false;
  bodyOut = sink.take();
  return true;
}

}  // namespace M4NativeProviderHttp
