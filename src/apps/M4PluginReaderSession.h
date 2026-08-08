#pragma once

// Thread-safe handoff queue between Lua owner task and AppRuntime UI task.

#include "util/M4PluginReaderBridge.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

namespace M4PluginReaderSession {

struct ProgressSnapshot {
  int page = 0;
  int total = -1;
  size_t byteOffset = 0;
  bool complete = false;
  char bookId[M4PluginReaderBridge::kMaxIdLen + 1] = {};
  char chapterUid[M4PluginReaderBridge::kMaxIdLen + 1] = {};
  char progressKey[M4PluginReaderBridge::kMaxProgressKeyLen + 1] = {};
  bool pendingDeliver = false;
  uint32_t generation = 0;
  // Non-empty when native open/load/encoding failed (not a successful close).
  char error[32] = {};
  bool openFailed = false;
  // When >= 0: user picked another book chapter from system TOC while reading.
  // 0-based index into the plugin's toc.json chapter list.
  int switchChapterIndex = -1;
};

inline std::mutex& mu() {
  static std::mutex m;
  return m;
}

inline M4PluginReaderBridge::OpenRequest& pendingOpen() {
  static M4PluginReaderBridge::OpenRequest r;
  return r;
}

inline std::atomic<bool>& openReady() {
  static std::atomic<bool> v{false};
  return v;
}

// Provider cross-chapter fallback. A native reader can discover that chapter
// N+1 is not locally openable yet (missing/fetching/empty cache). Historically
// it queued an OpenRequest with relPath="", which guaranteed one native open
// failure before Lua could show the downloader. Instead remember only the
// target chapter index; TxtReaderActivity converts requestPluginClose() into the
// same switchChapterIndex handoff used by the already-working chapter picker.
inline std::atomic<int>& fallbackSwitchChapterIndex() {
  static std::atomic<int> v{-1};
  return v;
}

inline int pendingFallbackSwitchChapterIndex() {
  return fallbackSwitchChapterIndex().load(std::memory_order_acquire);
}

// True from takeOpen until tryLaunch finishes enter/fail — blocks concurrent
// Lua displayBuffer that would race the native reader display task on e-ink.
inline std::atomic<bool>& launchInProgress() {
  static std::atomic<bool> v{false};
  return v;
}

inline std::atomic<uint32_t>& generation() {
  static std::atomic<uint32_t> g{1};
  return g;
}

inline ProgressSnapshot& progressSlot() {
  static ProgressSnapshot p;
  return p;
}

inline std::string& boundAppId() {
  static std::string id;
  return id;
}

inline uint32_t bumpGeneration() { return generation().fetch_add(1, std::memory_order_relaxed) + 1; }

// Native system chapter list (plugin WeRead TOC) handoff — same owner-task rules as openText.
struct TocRequest {
  std::string tocRelPath;
  std::string tocAbsPath;
  // File-backed ContentProvider catalogs may not have a JSON toc file.  In
  // that case the owner task resolves providerId + bookId against the
  // registry and materializes only the bounded native-picker title list.
  std::string providerId;
  std::string appDataRoot;
  std::string bookTitle;
  std::string bookId;
  std::string appId;
  int currentIndex = 0;  // 0-based
  uint32_t generation = 0;
};

struct TocResult {
  bool pendingDeliver = false;
  bool cancelled = true;
  int chapterIndex = -1;  // 0-based when selected
  char bookId[M4PluginReaderBridge::kMaxIdLen + 1] = {};
  uint32_t generation = 0;
};

inline TocRequest& pendingToc() {
  static TocRequest r;
  return r;
}

inline std::atomic<bool>& tocReady() {
  static std::atomic<bool> v{false};
  return v;
}

inline TocResult& tocResultSlot() {
  static TocResult r;
  return r;
}

inline void clearPendingOpen() {
  std::lock_guard<std::mutex> lock(mu());
  openReady().store(false, std::memory_order_relaxed);
  launchInProgress().store(false, std::memory_order_relaxed);
  pendingOpen() = {};
}

inline void clearPendingToc() {
  std::lock_guard<std::mutex> lock(mu());
  tocReady().store(false, std::memory_order_relaxed);
  launchInProgress().store(false, std::memory_order_relaxed);
  pendingToc() = {};
}

inline void clearProgress() {
  std::lock_guard<std::mutex> lock(mu());
  progressSlot() = {};
}

inline void clearForApp(const std::string& appId) {
  std::lock_guard<std::mutex> lock(mu());
  boundAppId() = appId;
  openReady().store(false, std::memory_order_relaxed);
  tocReady().store(false, std::memory_order_relaxed);
  launchInProgress().store(false, std::memory_order_relaxed);
  fallbackSwitchChapterIndex().store(-1, std::memory_order_release);
  pendingOpen() = {};
  pendingToc() = {};
  progressSlot() = {};
  tocResultSlot() = {};
  // Bump generation so in-flight readers become stale.
  generation().fetch_add(1, std::memory_order_relaxed);
}

inline bool queueOpen(const M4PluginReaderBridge::OpenRequest& req) {
  std::lock_guard<std::mutex> lock(mu());
  if (!boundAppId().empty() && req.appId != boundAppId()) {
    return false;
  }

  // Provider chapter-end fallback: an empty relPath is not an openable native
  // reader request. Treat it as a chapter-selection intent instead of poisoning
  // the owner queue with a guaranteed EmptyPath failure. The native reader will
  // close and publish this index through its normal progress snapshot; Lua then
  // enters the same loading/download flow as a manual chapter-list selection.
  if (!req.providerId.empty() && req.chapterIndex >= 0 && req.relPath.empty() && req.absPath.empty()) {
    openReady().store(false, std::memory_order_relaxed);
    pendingOpen() = {};
    tocReady().store(false, std::memory_order_relaxed);
    pendingToc() = {};
    fallbackSwitchChapterIndex().store(req.chapterIndex, std::memory_order_release);
    return true;
  }

  // A real open supersedes any old fallback intent.
  fallbackSwitchChapterIndex().store(-1, std::memory_order_release);
  // Prefer reader open; drop any pending TOC so we never dual-launch.
  tocReady().store(false, std::memory_order_relaxed);
  pendingToc() = {};
  pendingOpen() = req;
  openReady().store(true, std::memory_order_release);
  return true;
}

inline bool takeOpen(M4PluginReaderBridge::OpenRequest& out) {
  if (!openReady().load(std::memory_order_acquire)) return false;
  std::lock_guard<std::mutex> lock(mu());
  if (!openReady().load(std::memory_order_relaxed)) return false;
  out = pendingOpen();
  openReady().store(false, std::memory_order_relaxed);
  // Mark launch busy before releasing lock so the other thread cannot
  // displayBuffer between takeOpen and enterNewActivity.
  launchInProgress().store(true, std::memory_order_release);
  return true;
}

inline bool queueToc(const TocRequest& req) {
  std::lock_guard<std::mutex> lock(mu());
  if (!boundAppId().empty() && req.appId != boundAppId()) {
    return false;
  }
  fallbackSwitchChapterIndex().store(-1, std::memory_order_release);
  // Drop pending reader open if TOC is requested (shelf → TOC path).
  openReady().store(false, std::memory_order_relaxed);
  pendingOpen() = {};
  pendingToc() = req;
  tocReady().store(true, std::memory_order_release);
  return true;
}

inline bool takeToc(TocRequest& out) {
  if (!tocReady().load(std::memory_order_acquire)) return false;
  std::lock_guard<std::mutex> lock(mu());
  if (!tocReady().load(std::memory_order_relaxed)) return false;
  out = pendingToc();
  tocReady().store(false, std::memory_order_relaxed);
  launchInProgress().store(true, std::memory_order_release);
  return true;
}

inline void clearLaunchInProgress() {
  launchInProgress().store(false, std::memory_order_release);
}

// openText / openToc accepted or tryLaunch mid-flight: Lua must not paint/display.
// (Lua paint over native handoff races e-ink BUSY and leaves residual loading UI.)
inline bool handoffBlocksLuaDisplay() {
  return openReady().load(std::memory_order_acquire) ||
         tocReady().load(std::memory_order_acquire) ||
         launchInProgress().load(std::memory_order_acquire);
}

inline void publishTocResult(const TocResult& r) {
  std::lock_guard<std::mutex> lock(mu());
  tocResultSlot() = r;
  tocResultSlot().pendingDeliver = true;
}

inline bool takeTocResult(TocResult& out) {
  std::lock_guard<std::mutex> lock(mu());
  if (!tocResultSlot().pendingDeliver) return false;
  out = tocResultSlot();
  tocResultSlot().pendingDeliver = false;
  return true;
}

inline void publishProgress(const ProgressSnapshot& p) {
  std::lock_guard<std::mutex> lock(mu());
  progressSlot() = p;
  progressSlot().pendingDeliver = true;
}

// Deliver pending reader-close progress only for the active generation.
// A reader close can race a subsequent openText/openToc; accepting an older
// snapshot would write the previous chapter's progress into the new session.
// clearForApp() also clears the slot, but the generation check covers the
// narrower same-app handoff race.
inline bool takeProgress(ProgressSnapshot& out, uint32_t expectGeneration = 0) {
  std::lock_guard<std::mutex> lock(mu());
  if (!progressSlot().pendingDeliver) return false;
  const uint32_t gen = progressSlot().generation;
  const uint32_t cur = generation().load(std::memory_order_relaxed);
  if (expectGeneration != 0 && gen != expectGeneration) {
    progressSlot().pendingDeliver = false;
    return false;
  }
  if (gen == 0 || cur == 0 || gen != cur) {
    progressSlot().pendingDeliver = false;
    return false;
  }
  out = progressSlot();
  progressSlot().pendingDeliver = false;
  return true;
}

inline uint32_t currentGeneration() {
  return generation().load(std::memory_order_relaxed);
}

}  // namespace M4PluginReaderSession
