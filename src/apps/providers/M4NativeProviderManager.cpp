#include "apps/providers/M4NativeProviderManager.h"

#include "apps/M4ContentProviderCatalog.h"
#include "apps/M4ContentProviderSession.h"
#include "apps/providers/M4NativeProviderAdapters.h"
#include "apps/providers/M4NativeProviderIo.h"
#include "util/M4PluginReaderBridge.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SDCardManager.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

namespace M4NativeProviderManager {
namespace {

struct StoredBook {
  M4ContentProvider::BookSpec spec;
  std::string appDataRoot;
};

std::mutex gMu;
std::unordered_map<std::string, StoredBook> gBooks;
M4NativeProvider::Progress gProgress;
std::atomic<bool> gCancel{false};
TaskHandle_t gWorker = nullptr;

std::string keyOf(const std::string& p, const std::string& b) { return p + "\n" + b; }

std::string defaultAppId(const std::string& p) {
  if (p == "fanqie") return "com.fanqie.client";
  if (p == "jjwxc") return "com.jjwxc.client";
  if (p == "weread") return "com.weread.client";
  return {};
}

std::string appRoot(const std::string& appId) {
  return appId.empty() ? std::string() : std::string("/apps_data/") + appId;
}

std::string safeBookFile(const StoredBook& b) {
  if (b.appDataRoot.empty()) return {};
  return b.appDataRoot + "/provider/books/" + b.spec.bookId + "/book.json";
}

bool readSmall(const std::string& path, std::string& out, size_t cap = 32u * 1024u) {
  out.clear();
  FsFile f;
  if (!SdMan.openFileForRead("NP-STORE", path.c_str(), f)) return false;
  const size_t n = f.fileSize();
  if (n == 0 || n > cap) {
    f.close();
    return false;
  }
  out.resize(n);
  size_t off = 0;
  while (off < n) {
    const int r = f.read(reinterpret_cast<uint8_t*>(&out[off]), n - off);
    if (r <= 0) break;
    off += static_cast<size_t>(r);
  }
  f.close();
  if (off != n) {
    out.clear();
    return false;
  }
  return true;
}

bool writeExact(const std::string& path, const std::string& body) {
  if (!M4NativeProviderIo::ensureParentDirs(path)) return false;
  const std::string tmp = path + ".tmp";
  if (SdMan.exists(tmp.c_str())) SdMan.remove(tmp.c_str());
  FsFile f;
  if (!SdMan.openFileForWrite("NP-STORE", tmp.c_str(), f)) return false;
  size_t off = 0;
  while (off < body.size()) {
    const size_t n = std::min<size_t>(4096, body.size() - off);
    const int w = f.write(reinterpret_cast<const uint8_t*>(body.data() + off), n);
    if (w <= 0) {
      f.close();
      SdMan.remove(tmp.c_str());
      return false;
    }
    off += static_cast<size_t>(w);
  }
  f.close();
  if (SdMan.exists(path.c_str())) SdMan.remove(path.c_str());
  if (!SdMan.rename(tmp.c_str(), path.c_str())) {
    SdMan.remove(tmp.c_str());
    return false;
  }
  return true;
}

bool persist(const StoredBook& b) {
  if (b.spec.catalog.kind != M4ContentProvider::ChapterCatalogKind::FileRows) return false;
  JsonDocument doc;
  doc["version"] = 1;
  doc["providerId"] = b.spec.providerId;
  doc["appId"] = b.spec.appId;
  doc["bookId"] = b.spec.bookId;
  doc["title"] = b.spec.title;
  doc["currentIndex"] = b.spec.currentIndex0;
  JsonObject c = doc["catalog"].to<JsonObject>();
  c["path"] = b.spec.catalog.fileRelPath;
  c["count"] = b.spec.catalog.chapterCount;
  c["uidField"] = b.spec.catalog.uidField0;
  c["titleField"] = b.spec.catalog.titleField0;
  c["vipField"] = b.spec.catalog.vipField0;
  JsonObject cp = doc["cachePolicy"].to<JsonObject>();
  cp["maxChapterBytes"] = b.spec.cachePolicy.maxChapterBytes;
  cp["prefetchAhead"] = b.spec.cachePolicy.prefetchAhead;
  cp["retainBehind"] = b.spec.cachePolicy.retainBehind;
  cp["maxReadyChapters"] = b.spec.cachePolicy.maxReadyChapters;
  cp["offlineReopen"] = b.spec.cachePolicy.offlineReopen;
  std::string body;
  serializeJson(doc, body);
  return body.size() <= 16u * 1024u && writeExact(safeBookFile(b), body);
}

bool loadPersisted(const std::string& providerId, const std::string& bookId,
                   const std::string& appIdHint, StoredBook& out) {
  const std::string appId = appIdHint.empty() ? defaultAppId(providerId) : appIdHint;
  if (appId.empty()) return false;
  out = {};
  out.appDataRoot = appRoot(appId);
  out.spec.providerId = providerId;
  out.spec.bookId = bookId;
  out.spec.appId = appId;
  std::string raw;
  if (!readSmall(safeBookFile(out), raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw)) return false;
  if (std::string(doc["providerId"] | "") != providerId || std::string(doc["bookId"] | "") != bookId) return false;
  out.spec.title = doc["title"] | "";
  out.spec.currentIndex0 = doc["currentIndex"] | 0;
  JsonObject c = doc["catalog"].as<JsonObject>();
  out.spec.catalog.kind = M4ContentProvider::ChapterCatalogKind::FileRows;
  out.spec.catalog.fileRelPath = c["path"] | "";
  out.spec.catalog.chapterCount = c["count"] | 0;
  out.spec.catalog.uidField0 = c["uidField"] | 0;
  out.spec.catalog.titleField0 = c["titleField"] | 1;
  out.spec.catalog.vipField0 = c["vipField"] | -1;
  JsonObject cp = doc["cachePolicy"].as<JsonObject>();
  if (!cp.isNull()) {
    out.spec.cachePolicy.maxChapterBytes = cp["maxChapterBytes"] | (2u * 1024u * 1024u);
    out.spec.cachePolicy.prefetchAhead = cp["prefetchAhead"] | 1;
    out.spec.cachePolicy.retainBehind = cp["retainBehind"] | 1;
    out.spec.cachePolicy.maxReadyChapters = cp["maxReadyChapters"] | 4;
    out.spec.cachePolicy.offlineReopen = cp["offlineReopen"] | true;
  }
  return M4ContentProvider::validateBookSpec(out.spec) == M4ContentProvider::ValidationError::None;
}

size_t countLines(const std::string& path, size_t cap = M4ContentProvider::kMaxCatalogChapters) {
  FsFile f;
  if (!SdMan.openFileForRead("NP-TOC", path.c_str(), f)) return 0;
  uint8_t buf[2048];
  size_t rows = 0;
  bool any = false;
  uint8_t last = '\n';
  while (f.available()) {
    const int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    any = true;
    for (int i = 0; i < n; ++i) {
      last = buf[i];
      if (buf[i] == '\n' && ++rows >= cap) {
        f.close();
        return rows;
      }
    }
  }
  f.close();
  if (any && last != '\n' && rows < cap) ++rows;
  return rows;
}

bool inferLegacy(const std::string& providerId, const std::string& bookId,
                 const std::string& appIdHint, const std::string& titleHint, StoredBook& out) {
  const std::string appId = appIdHint.empty() ? defaultAppId(providerId) : appIdHint;
  if (appId.empty()) return false;
  out = {};
  out.appDataRoot = appRoot(appId);
  out.spec.providerId = providerId;
  out.spec.bookId = bookId;
  out.spec.appId = appId;
  out.spec.title = titleHint.empty() ? bookId : titleHint;
  out.spec.catalog.kind = M4ContentProvider::ChapterCatalogKind::FileRows;

  const std::string base = out.appDataRoot + "/cache/" + bookId;
  std::string raw;
  if (readSmall(base + "/toc_catalog.json", raw)) {
    JsonDocument doc;
    if (!deserializeJson(doc, raw)) {
      out.spec.catalog.fileRelPath = doc["source"] | (std::string("cache/") + bookId + "/toc_rows.txt");
      out.spec.catalog.chapterCount = doc["count"] | 0;
      out.spec.catalog.uidField0 = doc["uid_field"] | 0;
      out.spec.catalog.titleField0 = doc["title_field"] | 1;
    }
  }
  if (out.spec.catalog.fileRelPath.empty()) out.spec.catalog.fileRelPath = "cache/" + bookId + "/toc_rows.txt";
  if (out.spec.catalog.chapterCount == 0) {
    out.spec.catalog.chapterCount = countLines(out.appDataRoot + "/" + out.spec.catalog.fileRelPath);
  }
  if (providerId == "jjwxc") out.spec.catalog.vipField0 = 3;
  if (out.spec.catalog.chapterCount == 0) return false;
  out.spec.currentIndex0 = 0;
  if (M4ContentProvider::validateBookSpec(out.spec) != M4ContentProvider::ValidationError::None) return false;
  (void)persist(out);
  return true;
}

bool getBook(const std::string& providerId, const std::string& bookId, StoredBook& out) {
  std::lock_guard<std::mutex> lock(gMu);
  auto it = gBooks.find(keyOf(providerId, bookId));
  if (it == gBooks.end()) return false;
  out = it->second;
  return true;
}

bool readCatalogLine(const StoredBook& b, int index0, std::string& line) {
  line.clear();
  if (index0 < 0 || static_cast<size_t>(index0) >= b.spec.catalog.chapterCount) return false;
  const std::string path = b.appDataRoot + "/" + b.spec.catalog.fileRelPath;
  FsFile f;
  if (!SdMan.openFileForRead("NP-ROW", path.c_str(), f)) return false;
  constexpr size_t kMaxLine = 2048;
  int row = 0;
  uint8_t buf[1024];
  while (f.available()) {
    const int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    for (int i = 0; i < n; ++i) {
      const char c = static_cast<char>(buf[i]);
      if (row == index0) {
        if (c == '\n') {
          f.close();
          if (!line.empty() && line.back() == '\r') line.pop_back();
          return !line.empty();
        }
        if (line.size() >= kMaxLine) {
          f.close();
          return false;
        }
        line.push_back(c);
      } else if (c == '\n') {
        ++row;
        if (row > index0) {
          f.close();
          return false;
        }
      }
    }
  }
  f.close();
  return row == index0 && !line.empty();
}

bool resolveChapter(const StoredBook& b, int index0, M4ContentProvider::ChapterMeta& ch,
                    std::string& rawLine) {
  ch = {};
  rawLine.clear();
  if (b.spec.catalog.kind == M4ContentProvider::ChapterCatalogKind::Inline) {
    if (index0 < 0 || static_cast<size_t>(index0) >= b.spec.chapters.size()) return false;
    ch = b.spec.chapters[static_cast<size_t>(index0)];
    return true;
  }
  if (!readCatalogLine(b, index0, rawLine)) return false;
  if (!M4ContentProviderCatalog::fieldAt(rawLine, b.spec.catalog.uidField0, ch.uid) || ch.uid.empty()) return false;
  if (b.spec.catalog.titleField0 >= 0) {
    (void)M4ContentProviderCatalog::fieldAt(rawLine, b.spec.catalog.titleField0, ch.title);
  }
  return M4ContentProvider::idOk(ch.uid.c_str(), M4ContentProvider::kMaxChapterUidLen);
}

void updateProgress(const M4NativeProvider::Progress& p) {
  std::lock_guard<std::mutex> lock(gMu);
  gProgress = p;
}

void setPhase(const StoredBook& b, const M4ContentProvider::ChapterMeta& ch, int index0,
              M4NativeProvider::Phase phase, size_t recv, size_t written, int pct,
              const std::string& error = {}) {
  M4NativeProvider::Progress p;
  {
    std::lock_guard<std::mutex> lock(gMu);
    p = gProgress;
  }
  if (p.startedMs == 0 || p.providerId != b.spec.providerId || p.bookId != b.spec.bookId ||
      p.chapterIndex0 != index0) {
    p.startedMs = millis();
  }
  p.providerId = b.spec.providerId;
  p.bookId = b.spec.bookId;
  p.chapterUid = ch.uid;
  p.chapterIndex0 = index0;
  p.phase = phase;
  p.receivedBytes = recv;
  p.writtenBytes = written;
  p.percent = std::max(0, std::min(100, pct));
  p.updatedMs = millis();
  p.error = error;
  updateProgress(p);
}

void processWork(const M4ContentProvider::PrefetchWork& w) {
  StoredBook b;
  if (!getBook(w.providerId, w.bookId, b)) {
    if (!ensureBook(w.providerId, w.bookId) || !getBook(w.providerId, w.bookId, b)) return;
  }
  M4ContentProvider::ChapterMeta ch;
  std::string rawLine;
  if (!resolveChapter(b, w.index0, ch, rawLine)) {
    M4ContentProvider::ChapterStatus st;
    st.providerId = w.providerId;
    st.bookId = w.bookId;
    st.index0 = w.index0;
    st.state = M4ContentProvider::ChapterReady::Error;
    st.error = "catalog_resolve";
    (void)M4ContentProviderSession::setChapterStatus(st);
    setPhase(b, ch, w.index0, M4NativeProvider::Phase::Error, 0, 0, 0, st.error);
    return;
  }

  const std::string rel = chapterRelPath(b.spec.bookId, ch.uid);
  const std::string abs = b.appDataRoot + "/" + rel;
  size_t cached = 0;
  if (M4NativeProviderIo::cacheComplete(abs, &cached)) {
    M4ContentProvider::ChapterStatus st;
    st.providerId = b.spec.providerId;
    st.bookId = b.spec.bookId;
    st.chapterUid = ch.uid;
    st.index0 = w.index0;
    st.state = M4ContentProvider::ChapterReady::Ready;
    st.cacheRelPath = rel;
    st.pct = 100;
    (void)M4ContentProviderSession::setChapterStatus(st);
    setPhase(b, ch, w.index0, M4NativeProvider::Phase::Ready, 0, cached, 100);
    return;
  }

  M4ContentProvider::ChapterStatus fetching;
  fetching.providerId = b.spec.providerId;
  fetching.bookId = b.spec.bookId;
  fetching.chapterUid = ch.uid;
  fetching.index0 = w.index0;
  fetching.state = M4ContentProvider::ChapterReady::Fetching;
  fetching.pct = 1;
  (void)M4ContentProviderSession::setChapterStatus(fetching);
  setPhase(b, ch, w.index0, M4NativeProvider::Phase::Resolving, 0, 0, 1);

  auto adapter = M4NativeProviderAdapters::create(b.spec.providerId);
  if (!adapter) {
    fetching.state = M4ContentProvider::ChapterReady::Error;
    fetching.error = "provider_not_supported";
    (void)M4ContentProviderSession::setChapterStatus(fetching);
    setPhase(b, ch, w.index0, M4NativeProvider::Phase::Error, 0, 0, 0, fetching.error);
    return;
  }

  M4NativeProvider::ChapterRequest req;
  req.book = b.spec;
  req.chapter = ch;
  req.chapterIndex0 = w.index0;
  req.appDataRoot = b.appDataRoot;
  req.cacheRelPath = rel;
  req.cacheAbsPath = abs;
  req.catalogRawLine = rawLine;
  gCancel.store(false, std::memory_order_release);
  const auto result = adapter->fetchChapter(
      req,
      [&](M4NativeProvider::Phase phase, size_t recv, size_t written, int pct) {
        setPhase(b, ch, w.index0, phase, recv, written, pct);
        M4ContentProvider::ChapterStatus s = fetching;
        s.state = M4ContentProvider::ChapterReady::Fetching;
        s.pct = pct;
        (void)M4ContentProviderSession::setChapterStatus(s);
      },
      []() { return gCancel.load(std::memory_order_acquire); });

  M4ContentProvider::ChapterStatus st;
  st.providerId = b.spec.providerId;
  st.bookId = b.spec.bookId;
  st.chapterUid = ch.uid;
  st.index0 = w.index0;
  if (result.ok) {
    st.state = M4ContentProvider::ChapterReady::Ready;
    st.cacheRelPath = result.cacheRelPath.empty() ? rel : result.cacheRelPath;
    st.pct = 100;
    (void)M4ContentProviderSession::setChapterStatus(st);
    setPhase(b, ch, w.index0, M4NativeProvider::Phase::Ready, result.bytes, result.bytes, 100);
  } else {
    st.state = M4ContentProvider::ChapterReady::Error;
    st.error = result.error.empty() ? "fetch_failed" : result.error;
    (void)M4ContentProviderSession::setChapterStatus(st);
    setPhase(b, ch, w.index0,
             result.authRequired ? M4NativeProvider::Phase::AuthRequired
                                 : (st.error == "cancelled" ? M4NativeProvider::Phase::Cancelled
                                                            : M4NativeProvider::Phase::Error),
             0, 0, 0, st.error);
  }
}

void workerMain(void*) {
  uint32_t idleStarted = millis();
  while (true) {
    const auto w = M4ContentProviderSession::pollWork();
    if (w.valid && supports(w.providerId)) {
      idleStarted = millis();
      processWork(w);
      continue;
    }
    if (millis() - idleStarted >= 1200) {
      std::lock_guard<std::mutex> lock(gMu);
      if (M4ContentProviderSession::pendingWorkCount() == 0) {
        gWorker = nullptr;
        break;
      }
      idleStarted = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(40));
  }
  vTaskDelete(nullptr);
}

void kickWorker() {
  std::lock_guard<std::mutex> lock(gMu);
  if (gWorker) return;
  const BaseType_t ok = xTaskCreate(workerMain, "NativeProvider", 8192, nullptr, 1, &gWorker);
  if (ok != pdPASS) gWorker = nullptr;
}

}  // namespace

bool supports(const std::string& providerId) {
  return providerId == "fanqie" || providerId == "jjwxc" || providerId == "weread";
}

bool registerBook(const M4ContentProvider::BookSpec& spec) {
  if (!supports(spec.providerId) ||
      M4ContentProvider::validateBookSpec(spec) != M4ContentProvider::ValidationError::None || spec.appId.empty()) {
    return false;
  }
  StoredBook b;
  b.spec = spec;
  b.appDataRoot = appRoot(spec.appId);
  {
    std::lock_guard<std::mutex> lock(gMu);
    gBooks[keyOf(spec.providerId, spec.bookId)] = b;
  }
  (void)M4ContentProviderSession::registerBook(spec);
  const bool ok = persist(b);
  kickWorker();
  return ok;
}

bool ensureBook(const std::string& providerId, const std::string& bookId,
                const std::string& appId, const std::string& title) {
  if (!supports(providerId) || !M4ContentProvider::idOk(bookId.c_str(), M4ContentProvider::kMaxBookIdLen)) return false;
  {
    std::lock_guard<std::mutex> lock(gMu);
    if (gBooks.find(keyOf(providerId, bookId)) != gBooks.end()) return true;
  }
  StoredBook b;
  if (!loadPersisted(providerId, bookId, appId, b) && !inferLegacy(providerId, bookId, appId, title, b)) return false;
  if (!title.empty() && (b.spec.title.empty() || b.spec.title == b.spec.bookId)) b.spec.title = title;
  {
    std::lock_guard<std::mutex> lock(gMu);
    gBooks[keyOf(providerId, bookId)] = b;
  }
  (void)M4ContentProviderSession::registerBook(b.spec);
  kickWorker();
  return true;
}

bool ensureChapter(const std::string& providerId, const std::string& bookId, int index0, bool foreground) {
  if (!ensureBook(providerId, bookId)) return false;
  StoredBook b;
  if (!getBook(providerId, bookId, b) || index0 < 0 || static_cast<size_t>(index0) >= M4ContentProvider::bookChapterCount(b.spec)) {
    return false;
  }
  M4ContentProvider::ChapterMeta ch;
  std::string raw;
  if (!resolveChapter(b, index0, ch, raw)) return false;
  const std::string rel = chapterRelPath(bookId, ch.uid);
  const std::string abs = b.appDataRoot + "/" + rel;
  size_t n = 0;
  if (M4NativeProviderIo::cacheComplete(abs, &n)) {
    M4ContentProvider::ChapterStatus st;
    st.providerId = providerId;
    st.bookId = bookId;
    st.chapterUid = ch.uid;
    st.index0 = index0;
    st.state = M4ContentProvider::ChapterReady::Ready;
    st.cacheRelPath = rel;
    st.pct = 100;
    (void)M4ContentProviderSession::setChapterStatus(st);
    setPhase(b, ch, index0, M4NativeProvider::Phase::Ready, 0, n, 100);
    return true;
  }
  if (foreground) gCancel.store(false, std::memory_order_release);
  const bool queued = M4ContentProviderSession::requestPrefetch(providerId, bookId, index0);
  kickWorker();
  return queued;
}

M4NativeProvider::Progress progress() {
  std::lock_guard<std::mutex> lock(gMu);
  return gProgress;
}

void cancelForeground() { gCancel.store(true, std::memory_order_release); }

std::string appDataRootFor(const std::string& providerId, const std::string& bookId) {
  StoredBook b;
  if (getBook(providerId, bookId, b)) return b.appDataRoot;
  return appRoot(defaultAppId(providerId));
}

std::string chapterRelPath(const std::string& bookId, const std::string& chapterUid) {
  return std::string("cache/") + bookId + "/ch_" + chapterUid + ".txt";
}

void begin() { kickWorker(); }

}  // namespace M4NativeProviderManager
