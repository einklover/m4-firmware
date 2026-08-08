#include "apps/providers/M4NativeProvider.h"
#include "apps/providers/M4NativeProviderHttp.h"
#include "apps/providers/M4NativeProviderIo.h"

#include "apps/M4xPsvtsExtract.h"
#include "apps/weread/WereadCrypto.h"

#include <Arduino.h>
#include <SDCardManager.h>
#include <mbedtls/md5.h>
#include <esp_random.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <time.h>
#include <vector>

namespace M4NativeProviderAdapters {
namespace {

class DirectFileSink final : public M4xJsonStream::Sink {
 public:
  ~DirectFileSink() override { close(); }
  bool open(const std::string& path) {
    close();
    path_ = path;
    M4NativeProviderIo::ensureParentDirs(path_);
    if (SdMan.exists(path_.c_str())) SdMan.remove(path_.c_str());
    open_ = SdMan.openFileForWrite("WR-TMP", path_.c_str(), f_);
    return open_;
  }
  bool write(const uint8_t* data, size_t len) override {
    if (!open_ || !data) return false;
    if (len == 0) return true;
    const int n = f_.write(data, len);
    if (n != static_cast<int>(len)) return false;
    bytes_ += len;
    return true;
  }
  void close() {
    if (open_) {
      f_.close();
      open_ = false;
    }
  }
  size_t bytes() const { return bytes_; }
 private:
  FsFile f_;
  std::string path_;
  size_t bytes_ = 0;
  bool open_ = false;
};

class PsvtsSink final : public M4xJsonStream::Sink {
 public:
  PsvtsSink() { scanner_.reset(M4xPsvts::kMaxValueLen); }
  bool write(const uint8_t* data, size_t len) override {
    scanned_ += len;
    if (scanned_ > M4xPsvts::kMaxScanBytes) return false;
    if (!scanner_.found && !scanner_.valueTooLarge) scanner_.feed(data, len);
    return !scanner_.valueTooLarge;
  }
  bool found() const { return scanner_.found && !scanner_.value.empty(); }
  const std::string& value() const { return scanner_.value; }
 private:
  M4xPsvts::Scanner scanner_;
  size_t scanned_ = 0;
};

std::vector<M4NativeProviderHttp::Header> authHeaders(const std::string& cookie,
                                                       const std::string& referer,
                                                       bool json = false) {
  std::vector<M4NativeProviderHttp::Header> h;
  h.push_back({"Cookie", cookie});
  h.push_back({"Referer", referer});
  if (json) h.push_back({"Content-Type", "application/json"});
  return h;
}

std::string readPrefix(const std::string& path, size_t cap = 512) {
  FsFile f;
  if (!SdMan.openFileForRead("WR-PFX", path.c_str(), f)) return {};
  const size_t n = std::min(cap, static_cast<size_t>(f.fileSize()));
  std::string s;
  s.resize(n);
  const int r = n ? f.read(reinterpret_cast<uint8_t*>(&s[0]), n) : 0;
  f.close();
  if (r < 0 || static_cast<size_t>(r) != n) return {};
  return s;
}

bool containsLoginTimeout(const std::string& prefix) {
  return prefix.find("-2012") != std::string::npos || prefix.find("LOGIN_TIMEOUT") != std::string::npos;
}

bool fileSize(const std::string& path, size_t& n) {
  n = 0;
  FsFile f;
  if (!SdMan.openFileForRead("WR-SZ", path.c_str(), f)) return false;
  n = f.fileSize();
  f.close();
  return true;
}

std::string upperHex(const uint8_t md[16]) {
  char hex[33];
  for (int i = 0; i < 16; ++i) std::snprintf(hex + i * 2, 3, "%02X", md[i]);
  hex[32] = 0;
  return std::string(hex);
}

bool appendCheckedShard(FsFile& combined, const std::string& shardPath, std::string& err) {
  FsFile in;
  if (!SdMan.openFileForRead("WR-SHARD", shardPath.c_str(), in)) {
    err = "shard_open";
    return false;
  }
  const size_t total = in.fileSize();
  if (total <= 32) {
    in.close();
    err = "shard_short";
    return false;
  }
  char expectedRaw[32];
  if (in.read(reinterpret_cast<uint8_t*>(expectedRaw), sizeof(expectedRaw)) != 32) {
    in.close();
    err = "shard_header";
    return false;
  }
  std::string expected(expectedRaw, 32);
  std::transform(expected.begin(), expected.end(), expected.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });

  mbedtls_md5_context ctx;
  mbedtls_md5_init(&ctx);
  if (mbedtls_md5_starts(&ctx) != 0) {
    mbedtls_md5_free(&ctx);
    in.close();
    err = "md5_init";
    return false;
  }
  uint8_t buf[4096];
  size_t left = total - 32;
  bool ok = true;
  while (left > 0) {
    const size_t want = std::min(left, sizeof(buf));
    const int n = in.read(buf, want);
    if (n <= 0) {
      ok = false;
      break;
    }
    mbedtls_md5_update(&ctx, buf, static_cast<size_t>(n));
    if (combined.write(buf, static_cast<size_t>(n)) != n) {
      ok = false;
      break;
    }
    left -= static_cast<size_t>(n);
  }
  uint8_t md[16] = {};
  if (ok && mbedtls_md5_finish(&ctx, md) != 0) ok = false;
  mbedtls_md5_free(&ctx);
  in.close();
  if (!ok) {
    err = "shard_io";
    return false;
  }
  if (upperHex(md) != expected) {
    err = "shard_md5";
    return false;
  }
  return true;
}

std::vector<int> swapPositions(const uint8_t* tail, int tailN, int length) {
  std::vector<int> result;
  if (length < 4) return result;
  if (length < 11) return {0, 2};
  std::string tmp;
  for (int i = tailN - 1; i >= 0; --i) {
    const uint8_t v = tail[i];
    uint32_t val = 0;
    for (int b = 0; b < 8; ++b) if ((v >> b) & 1) val += (1u << (2 * b));
    tmp += std::to_string(val);
  }
  const int m = length - tailN - 2;
  if (m <= 0) return result;
  const int step = static_cast<int>(std::to_string(m).size());
  for (int i = 0; static_cast<int>(result.size()) < 10 && i + step < static_cast<int>(tmp.size()); i += step) {
    auto parse = [&](int at) {
      int v = 0;
      for (int k = 0; k < step && at + k < static_cast<int>(tmp.size()); ++k) {
        const char c = tmp[static_cast<size_t>(at + k)];
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (c - '0');
      }
      return v % m;
    };
    result.push_back(parse(i));
    result.push_back(parse(i + 1));
  }
  return result;
}

bool readByteAt(FsFile& f, size_t off, uint8_t& b) {
  return f.seek(off) && f.read(&b, 1) == 1;
}

bool writeByteAt(FsFile& f, size_t off, uint8_t b) {
  return f.seek(off) && f.write(&b, 1) == 1;
}

bool reverseSwapsOnFile(const std::string& path, size_t payloadBytes, std::string& err) {
  if (payloadBytes < 2) {
    err = "payload_short";
    return false;
  }
  const int length = static_cast<int>(payloadBytes - 1);  // enc starts after payload[0]
  const int n = std::min(4, (length + 9) / 10);
  FsFile f = SdMan.open(path.c_str(), O_RDWR);
  if (!f) {
    err = "swap_open";
    return false;
  }
  uint8_t tail[4] = {};
  for (int i = 0; i < n; ++i) {
    if (!readByteAt(f, 1u + static_cast<size_t>(length - n + i), tail[i])) {
      f.close();
      err = "swap_tail";
      return false;
    }
  }
  const auto pos = swapPositions(tail, n, length);
  for (int i = static_cast<int>(pos.size()) - 1; i > 0; i -= 2) {
    for (int k = 1; k >= 0; --k) {
      const int l = pos[static_cast<size_t>(i)] + k;
      const int r = pos[static_cast<size_t>(i - 1)] + k;
      if (l < 0 || r < 0 || l >= length || r >= length) continue;
      uint8_t a = 0, b = 0;
      if (!readByteAt(f, 1u + static_cast<size_t>(l), a) ||
          !readByteAt(f, 1u + static_cast<size_t>(r), b) ||
          !writeByteAt(f, 1u + static_cast<size_t>(l), b) ||
          !writeByteAt(f, 1u + static_cast<size_t>(r), a)) {
        f.close();
        err = "swap_io";
        return false;
      }
    }
  }
  f.flush();
  f.close();
  return true;
}

class XhtmlStripSink final : public M4xJsonStream::Sink {
 public:
  explicit XhtmlStripSink(M4xJsonStream::Sink& out) : out_(out) {}
  bool write(const uint8_t* data, size_t len) override {
    for (size_t i = 0; i < len; ++i) {
      const uint8_t b = data[i];
      if (inTag_) {
        if (b == '>') {
          std::string low = tag_;
          std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
          });
          if (low.find("br") == 0 || low.find("/p") == 0 || low.find("/div") == 0 || low.find("/li") == 0) {
            if (!emit('\n')) return false;
          }
          inTag_ = false;
          tag_.clear();
        } else if (b < 0x80 && tag_.size() < 32) {
          tag_.push_back(static_cast<char>(b));
        }
        continue;
      }
      if (inEntity_) {
        if (b == ';') {
          if (entity_ == "nbsp" || entity_ == "#160") { if (!emit(' ')) return false; }
          else if (entity_ == "amp") { if (!emit('&')) return false; }
          else if (entity_ == "lt") { if (!emit('<')) return false; }
          else if (entity_ == "gt") { if (!emit('>')) return false; }
          else { if (!emit(' ')) return false; }
          inEntity_ = false;
          entity_.clear();
          continue;
        }
        if (b < 0x80 && entity_.size() < 14) {
          entity_.push_back(static_cast<char>(b));
          continue;
        }
        if (!emit('&') || !out_.write(reinterpret_cast<const uint8_t*>(entity_.data()), entity_.size())) return false;
        inEntity_ = false;
        entity_.clear();
      }
      if (b == '<') {
        inTag_ = true;
        tag_.clear();
      } else if (b == '&') {
        inEntity_ = true;
        entity_.clear();
      } else if (b != '\r') {
        if (!out_.write(&b, 1)) return false;
      }
    }
    return true;
  }
 private:
  bool emit(char c) {
    const uint8_t b = static_cast<uint8_t>(c);
    return out_.write(&b, 1);
  }
  M4xJsonStream::Sink& out_;
  bool inTag_ = false;
  bool inEntity_ = false;
  std::string tag_;
  std::string entity_;
};

int b64Value(uint8_t c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-' || c == '+') return 62;
  if (c == '_' || c == '/') return 63;
  return -1;
}

bool decodeBase64File(const std::string& combinedPath, bool stripXhtml,
                      M4NativeProviderIo::PartFileSink& finalSink,
                      const M4NativeProvider::ProgressFn& progress,
                      const M4NativeProvider::CancelFn& cancelled,
                      std::string& err) {
  size_t total = 0;
  if (!fileSize(combinedPath, total) || total <= 1) {
    err = "payload_empty";
    return false;
  }
  if (!reverseSwapsOnFile(combinedPath, total, err)) return false;

  FsFile f;
  if (!SdMan.openFileForRead("WR-B64", combinedPath.c_str(), f) || !f.seek(1)) {
    err = "payload_open";
    if (f.isOpen()) f.close();
    return false;
  }
  XhtmlStripSink stripped(finalSink);
  M4xJsonStream::Sink& target = stripXhtml ? static_cast<M4xJsonStream::Sink&>(stripped)
                                           : static_cast<M4xJsonStream::Sink&>(finalSink);
  uint8_t in[4096];
  uint8_t out[3072 + 8];
  size_t outN = 0;
  int val = 0;
  int valb = -8;
  size_t done = 1;
  while (done < total) {
    if (cancelled && cancelled()) {
      f.close();
      err = "cancelled";
      return false;
    }
    const size_t want = std::min(sizeof(in), total - done);
    const int nr = f.read(in, want);
    if (nr <= 0) break;
    for (int i = 0; i < nr; ++i) {
      const uint8_t c = in[i];
      if (c == '=') {
        done = total;
        break;
      }
      const int d = b64Value(c);
      if (d < 0) continue;
      val = (val << 6) + d;
      valb += 6;
      if (valb >= 0) {
        out[outN++] = static_cast<uint8_t>((val >> valb) & 0xFF);
        valb -= 8;
        if (outN >= 3072) {
          if (!target.write(out, outN)) {
            f.close();
            err = "decode_write";
            return false;
          }
          outN = 0;
        }
      }
    }
    if (done != total) done += static_cast<size_t>(nr);
    if (progress) {
      const int pct = static_cast<int>((std::min(done, total) * 90u) / total);
      progress(M4NativeProvider::Phase::Decoding, total, finalSink.written(), pct);
    }
  }
  f.close();
  if (outN && !target.write(out, outN)) {
    err = "decode_write";
    return false;
  }
  return finalSink.written() > 0;
}

bool combineShards(const std::vector<std::string>& shards, const std::string& combinedPath,
                   size_t& payloadBytes, std::string& err) {
  payloadBytes = 0;
  if (SdMan.exists(combinedPath.c_str())) SdMan.remove(combinedPath.c_str());
  M4NativeProviderIo::ensureParentDirs(combinedPath);
  FsFile out;
  if (!SdMan.openFileForWrite("WR-COMB", combinedPath.c_str(), out)) {
    err = "combine_open";
    return false;
  }
  for (const auto& p : shards) {
    if (p.empty()) continue;
    if (!appendCheckedShard(out, p, err)) {
      out.close();
      SdMan.remove(combinedPath.c_str());
      return false;
    }
  }
  out.flush();
  payloadBytes = out.fileSize();
  out.close();
  if (payloadBytes <= 1) {
    SdMan.remove(combinedPath.c_str());
    err = "payload_empty";
    return false;
  }
  return true;
}

class WereadProvider final : public M4NativeProvider::Adapter {
 public:
  const char* id() const override { return "weread"; }

  M4NativeProvider::FetchResult fetchChapter(const M4NativeProvider::ChapterRequest& req,
                                             const M4NativeProvider::ProgressFn& progress,
                                             const M4NativeProvider::CancelFn& cancelled) override {
    M4NativeProvider::FetchResult out;
    size_t cached = 0;
    if (M4NativeProviderIo::cacheComplete(req.cacheAbsPath, &cached)) {
      out.ok = true;
      out.bytes = cached;
      out.cacheRelPath = req.cacheRelPath;
      if (progress) progress(M4NativeProvider::Phase::Ready, 0, cached, 100);
      return out;
    }

    std::string cookie;
    if (!M4NativeProviderIo::loadCookieHeader(req.appDataRoot, "weread", cookie)) {
      out.authRequired = true;
      out.error = "login_required";
      return out;
    }

    const std::string readerUrl = std::string("https://weread.qq.com/web/reader/") +
                                  weread_crypto::e(req.book.bookId) + "k" +
                                  weread_crypto::e(req.chapter.uid);
    std::string psvts;
    if (!fetchPsvts(readerUrl, cookie, psvts, cancelled, out.error)) {
      if (out.error == "login_required") out.authRequired = true;
      return out;
    }

    const std::string base = req.cacheAbsPath + ".wr";
    const std::string e0 = base + ".e0";
    const std::string a = base + ".a";
    const std::string b = base + ".b";
    const std::string combined = base + ".combined";
    auto cleanup = [&]() {
      const char* paths[] = {e0.c_str(), a.c_str(), b.c_str(), combined.c_str()};
      for (const char* p : paths) if (SdMan.exists(p)) SdMan.remove(p);
    };
    cleanup();

    if (progress) progress(M4NativeProvider::Phase::Connecting, 0, 0, 0);
    if (!downloadShard("/web/book/chapter/e_0", readerUrl, req, psvts, cookie, e0,
                       progress, cancelled, out.error)) {
      if (out.error == "login_required") out.authRequired = true;
      cleanup();
      return out;
    }
    const std::string pfx = readPrefix(e0);
    if (containsLoginTimeout(pfx)) {
      out.authRequired = true;
      out.error = "login_required";
      cleanup();
      return out;
    }
    if (pfx.size() >= 2 && pfx[0] == 'P' && pfx[1] == 'K') {
      out.error = "epub_zip_unsupported";
      cleanup();
      return out;
    }
    if (pfx == "{}" || pfx.empty()) {
      out.error = "empty_content";
      cleanup();
      return out;
    }

    const bool textMode = pfx[0] == '{' && pfx.find("\"bookId\"") != std::string::npos;
    std::vector<std::string> shards;
    if (textMode) {
      if (!downloadShard("/web/book/chapter/t_0", readerUrl, req, psvts, cookie, a,
                         progress, cancelled, out.error) ||
          !downloadShard("/web/book/chapter/t_1", readerUrl, req, psvts, cookie, b,
                         progress, cancelled, out.error)) {
        cleanup();
        return out;
      }
      shards = {a, b};
      // e0 is routing metadata for text mode, not a content shard.
      SdMan.remove(e0.c_str());
    } else {
      if (!downloadShard("/web/book/chapter/e_1", readerUrl, req, psvts, cookie, a,
                         progress, cancelled, out.error) ||
          !downloadShard("/web/book/chapter/e_3", readerUrl, req, psvts, cookie, b,
                         progress, cancelled, out.error)) {
        cleanup();
        return out;
      }
      shards = {e0, a, b};
    }

    size_t payloadBytes = 0;
    if (progress) progress(M4NativeProvider::Phase::Decoding, 0, 0, 0);
    if (!combineShards(shards, combined, payloadBytes, out.error)) {
      cleanup();
      return out;
    }

    M4NativeProviderIo::PartFileSink finalSink;
    if (!finalSink.open(req.cacheAbsPath)) {
      out.error = "sd_open_failed";
      cleanup();
      return out;
    }
    if (!decodeBase64File(combined, !textMode, finalSink, progress, cancelled, out.error) ||
        !finalSink.flush() || finalSink.written() == 0) {
      finalSink.close();
      M4NativeProviderIo::removeIncomplete(req.cacheAbsPath);
      cleanup();
      if (out.error.empty()) out.error = "decode_failed";
      return out;
    }
    finalSink.close();
    size_t finalBytes = 0;
    if (!M4NativeProviderIo::commitPart(req.cacheAbsPath, &finalBytes)) {
      out.error = "cache_commit_failed";
      cleanup();
      return out;
    }
    cleanup();
    out.ok = true;
    out.bytes = finalBytes;
    out.cacheRelPath = req.cacheRelPath;
    if (progress) progress(M4NativeProvider::Phase::Ready, payloadBytes, finalBytes, 100);
    return out;
  }

 private:
  bool fetchPsvts(const std::string& readerUrl, const std::string& cookie, std::string& psvts,
                  const M4NativeProvider::CancelFn& cancelled, std::string& err) {
    PsvtsSink sink;
    M4NativeProviderHttp::Request r;
    r.url = readerUrl;
    r.headers = authHeaders(cookie, "https://weread.qq.com/");
    r.maxBytes = M4xPsvts::kMaxScanBytes;
    r.timeoutMs = 30000;
    const auto net = M4NativeProviderHttp::requestToSink(r, sink, {}, cancelled);
    if (!net.ok) {
      err = (net.status == 401 || net.status == 403) ? "login_required" : net.error;
      return false;
    }
    if (!sink.found()) {
      err = "psvts_not_found";
      return false;
    }
    psvts = sink.value();
    return true;
  }

  bool downloadShard(const std::string& endpoint, const std::string& readerUrl,
                     const M4NativeProvider::ChapterRequest& req, const std::string& psvts,
                     const std::string& cookie, const std::string& outPath,
                     const M4NativeProvider::ProgressFn& progress,
                     const M4NativeProvider::CancelFn& cancelled, std::string& err) {
    DirectFileSink sink;
    if (!sink.open(outPath)) {
      err = "sd_open_failed";
      return false;
    }
    M4NativeProviderHttp::Request r;
    r.method = "POST";
    r.url = std::string("https://weread.qq.com") + endpoint;
    r.headers = authHeaders(cookie, readerUrl, true);
    const long now = static_cast<long>(time(nullptr));
    const long rnd = static_cast<long>(esp_random() % 10000u);
    r.body = weread_crypto::makeContentParamsJson(req.book.bookId, req.chapter.uid, psvts,
                                                  false, 1, now, rnd);
    r.maxBytes = std::max<size_t>(2u * 1024u * 1024u, req.book.cachePolicy.maxChapterBytes * 2u);
    r.timeoutMs = 30000;
    const auto net = M4NativeProviderHttp::requestToSink(
        r, sink,
        [&](size_t n) {
          if (progress) progress(M4NativeProvider::Phase::Receiving, n, 0, 0);
        }, cancelled);
    sink.close();
    if (!net.ok || sink.bytes() == 0) {
      err = (net.status == 401 || net.status == 403) ? "login_required" : net.error;
      if (err.empty()) err = "shard_download";
      return false;
    }
    const std::string prefix = readPrefix(outPath, 256);
    if (containsLoginTimeout(prefix)) {
      err = "login_required";
      return false;
    }
    return true;
  }
};

}  // namespace

std::unique_ptr<M4NativeProvider::Adapter> createWereadProvider() {
  return std::make_unique<WereadProvider>();
}

}  // namespace M4NativeProviderAdapters
