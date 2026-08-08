#pragma once

#include "apps/M4xJsonStream.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace M4NativeProviderHttp {

struct Header {
  std::string name;
  std::string value;
};

struct Request {
  std::string method = "GET";
  std::string url;
  std::vector<Header> headers;
  std::string body;
  size_t maxBytes = 4u * 1024u * 1024u;
  uint32_t timeoutMs = 30000;
  bool followRedirects = false;
  // Public mirrors may opt out of CA validation. Credential-bearing providers
  // must leave this false.
  bool insecureTls = false;
};

struct Result {
  bool ok = false;
  int status = 0;
  size_t bytes = 0;
  std::string error;
  std::vector<Header> responseHeaders;
};

using ProgressFn = std::function<void(size_t bytes)>;
using CancelFn = std::function<bool()>;

// Bridges a streaming HTTP body into a ScalarStreamExtractor: requestToSink
// feeds raw bytes to the extractor, which parses the JSON and writes only the
// target scalar into its own Sink (file).
class ExtractorSink final : public M4xJsonStream::Sink {
 public:
  ExtractorSink(M4xJsonStream::ScalarStreamExtractor& extractor) : extractor_(extractor) {}
  bool write(const uint8_t* data, size_t len) override { return extractor_.feed(data, len); }

 private:
  M4xJsonStream::ScalarStreamExtractor& extractor_;
};

// Synchronous, bounded-body streaming request intended to run on the single
// native provider worker task. The body is never accumulated: HTTPClient
// decodes transfer framing and writes directly into SinkStream -> Sink.
Result requestToSink(const Request& req, M4xJsonStream::Sink& sink,
                     const ProgressFn& progress = {}, const CancelFn& cancelled = {});

// Small response helper for protocol metadata (psvts/login gate). Hard capped.
bool requestSmall(const Request& req, std::string& bodyOut, Result& resultOut,
                  size_t hardCap = 16u * 1024u, const CancelFn& cancelled = {});

}  // namespace M4NativeProviderHttp
