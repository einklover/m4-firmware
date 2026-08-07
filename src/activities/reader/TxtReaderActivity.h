#pragma once

#include <Txt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "EpubReaderMenuActivity.h"
#include "activities/ActivityWithSubactivity.h"
#include "util/M4ContentProviderContract.h"

class TxtReaderActivity final : public ActivityWithSubactivity {
 public:
  // Plugin / WeRead transient session: same native UI as library TXT reader,
  // without polluting Recent Books or treating the file as a multi-chapter book.
  struct PluginSession {
    bool active = false;
    bool suppressRecentBooks = true;
    bool suppressOpenEpubPath = true;
    bool progressiveIndex = true;
    std::string bookId;
    std::string chapterUid;
    std::string progressKey;
    std::string titleOverride;
    uint32_t generation = 0;
    size_t initialByteOffset = 0;
    bool hasInitialByteOffset = false;
    // App-data relative toc.json + current 0-based chapter for system TOC reuse.
    std::string tocRelPath;
    std::string tocAbsPath;
    int chapterIndex = 0;
    // ContentProvider-managed book (provider-agnostic; not a filesystem path).
    bool providerManaged = false;
    std::string providerId;    // e.g. "weread" (URI segment, NOT app id)
    std::string appId;         // installed m4x id e.g. "com.weread.client" — history author field
    std::string appDataRoot;   // /apps_data/<appId>
    std::string cacheRelPath;  // current chapter relative .txt
  };

  struct PluginProgress {
    // valid=false ⇒ lock not acquired or no coherent index; do NOT persist.
    bool valid = false;
    int page = 0;     // 0-based
    int total = -1;   // -1 while index incomplete
    size_t byteOffset = 0;
    bool indexComplete = false;
    uint32_t generation = 0;  // exact session generation (not live counter)
    std::string bookId;
    std::string chapterUid;
    std::string progressKey;
    // >=0: user selected another chapter from system TOC (0-based).
    int switchChapterIndex = -1;
  };

  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt,
                             const std::function<void()>& onGoBack, const std::function<void()>& onGoHome)
      : ActivityWithSubactivity("TxtReader", renderer, mappedInput),
        txt(std::move(txt)),
        onGoBack(onGoBack),
        onGoHome(onGoHome) {}

  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt,
                             const std::function<void()>& onGoBack, const std::function<void()>& onGoHome,
                             PluginSession pluginSession)
      : ActivityWithSubactivity("TxtReader", renderer, mappedInput),
        txt(std::move(txt)),
        onGoBack(onGoBack),
        onGoHome(onGoHome),
        pluginSession_(std::move(pluginSession)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override { return automaticPageTurnActive; }
  bool isReaderActivity() const override { return true; }

  // Parent observes after child loop returns (do not delete from onGoBack).
  bool pluginCloseRequested() const { return pluginCloseRequested_; }
  // Blocks until a coherent snapshot is available (may wait on e-paper/index).
  // Never returns a fabricated page-0 default on timeout — valid=false if impossible.
  PluginProgress pluginProgressSnapshot() const;
  bool pluginFirstPageReady() const;
  bool pluginIndexComplete() const;

 private:
  std::shared_ptr<Txt> txt;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int currentPage = 0;
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;
  bool updateRequired = false;
  bool pendingGoBack = false;
  bool pendingGoHome = false;
  const std::function<void()> onGoBack;
  const std::function<void()> onGoHome;

  std::vector<size_t> pageOffsets;
  std::vector<std::string> currentPageLines;
  std::vector<int> currentPageIndentOffsets;
  std::vector<bool> currentPageJustify;
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;

  int cachedFontId = 0;
  int cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;

  static constexpr int DUAL_PAGE_GUTTER = 8;
  bool isLandscapeDualPage() const;
  int dualRightPage = -1;
  bool dualNextLeft = true;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();
  void renderDualPage();
  void renderPage(bool skipDisplay = false, int xOffset = 0, bool skipInvert = false);
  void renderStatusBar(int orientedMarginRight, int orientedMarginBottom, int orientedMarginTop,
                       int orientedMarginLeft) const;

  // maxReadBytes: exclusive read-window size from offset (0 = default CHUNK_SIZE 8KiB).
  // First-page adaptive path passes growing 8/16/.../48KiB so expansion is real, not a re-read of 8KiB.
  // outJustify: if non-null, write per-line justify flags there; if null, write currentPageJustify.
  // Index builders MUST pass a scratch vector so progressive index does not clobber the
  // on-screen page's justify flags (causes mid-read left/justify flicker).
  bool loadPageAtOffset(size_t offset, size_t endoffset, std::vector<std::string>& outLines, size_t& nextOffset,
                        const uint8_t* preloadBuf = nullptr, size_t preloadBufOffset = 0, size_t preloadBufSize = 0,
                        size_t maxReadBytes = 0, std::vector<bool>* outJustify = nullptr);
  void buildPageIndex(size_t beginByte, size_t endByte);

  // Progressive indexing (plugin whole-file mode).
  void buildPageIndexFirstPage(size_t beginByte, size_t endByteExclusive);
  int continuePageIndex(int maxPages, size_t maxBytes);
  size_t chapterContentEnd() const;  // exclusive end for loadPageAtOffset

  void saveProgress() const;
  void loadProgress();
  int chapternum = 0;
  bool chapter_loadPageIndexCache(int chapternum);
  void chapter_savePageIndexCache(int chapternum) const;
  void chapter_initializeReader(int chapternum);
  bool chapter_initialized = false;
  bool needIndent = SETTINGS.firstlineintented;
  int8_t wordSpacing = SETTINGS.wordSpacing;

  void openMenu();
  void handleMenuAction(EpubReaderMenuActivity::MenuAction action);
  void onSettingsChanged();
  // UI/menu path: acquires the state lock then jumps.
  void goToPercent(int percent);
  // Display/render path: REQUIRES state lock already held (non-recursive mutex).
  // Never call from unlocked code; never call goToPercent while holding the lock.
  void goToPercentAlreadyLocked(int percent);

  unsigned long lastAutoPageTurnTime = 0;
  bool automaticPageTurnActive = false;
  unsigned long pageTurnDuration = 0;
  bool rollingMode = false;
  bool rollingHalfTurned = false;
  void applyAutoPageTurnSettings();

  bool pendingBluetoothSettings = false;
  bool globalNextPageMode = false;
  bool globalNextPageModeToggled = false;
  bool skipNextButtonCheck = false;
  int cachedPage = -1;
  int m_pendingJumpPercent = -1;

  // --- Plugin session ---
  PluginSession pluginSession_{};
  bool pluginCloseRequested_ = false;
  // Set when plugin TOC selects another chapter; published in pluginProgressSnapshot.
  int pluginSwitchChapterIndex_ = -1;
  bool firstPageReady_ = false;
  bool indexComplete_ = true;
  size_t indexRangeEnd_ = 0;   // exclusive file end for progressive index
  size_t indexCursor_ = 0;     // next page-start to discover
  uint32_t lastStatusRefreshMs_ = 0;
  size_t pendingRestoreByte_ = 0;
  bool hasPendingRestore_ = false;
  bool userMovedPage_ = false;
  bool tidxSaved_ = false;  // save completed .tidx once per layout generation
  // First physical paint after openText handoff: layout under lock, then
  // HALF_REFRESH outside the lock (absolute both-plane write — FAST is
  // differential and keeps residual Lua "打开阅读器…" when RED is stale).
  bool pluginNeedsClearRefresh_ = false;
  bool pluginPendingHalfFlush_ = false;
  // Provider next-chapter overlay (footer/status); empty when idle.
  std::string providerOverlayMsg_;
  // Last overlay state that drove a physical refresh. Only a state transition
  // (Missing→Fetching→Error/Ready) repaints the panel; pct-only churn updates
  // providerOverlayMsg_ in memory without a full-frame differential.
  M4ContentProvider::ChapterReady providerOverlayState_ = M4ContentProvider::ChapterReady::Ready;
  bool providerPrefetchRequested_ = false;
  bool tryProviderNextChapterAdvance();  // last-page next / seamless open
  void providerIdlePrefetchNext();
  bool switchToProviderChapter(const std::string& cacheRelPath, int index0, const std::string& chapterUid,
                               const std::string& title);
  // Library path: layout under lock only; e-ink + AA must run unlocked so the
  // main loop is not frozen for ~1.5s per page (see open hang serial analysis).
  bool deferPhysicalEpd_ = false;
  bool libraryPhysicalPending_ = false;
  // True while finishPhysicalDisplay / plugin half is on the panel (SPI busy).
  // Display task vs UI task: atomic, not volatile (ordering + visibility).
  std::atomic<bool> physicalEpdBusy_{false};
  bool firstPhysicalShown_ = false;  // first content page has been driven to panel
  // Enter/return to reader: flush pure white first so page-turn anim and FAST
  // never diff against the previous activity (shelf/menu/loading residual).
  bool entryWhiteSeedPending_ = false;
  // Last body page that received a physical EPD drive. Same-page buffer updates
  // (status "1/?"→"1/20", footer overlay churn) must NOT FAST-diff again —
  // that was the residual/ghost buildup while progressive index ran.
  int lastPhysicalBodyPage_ = -1;
  // Set on onExit / openMenu so display task stops starting new frames.
  std::atomic<bool> suppressDisplay_{false};
  void finishPhysicalDisplay();  // displayBuffer + optional AA (no state lock)
  void waitPhysicalEpdIdle(uint32_t maxMs = 2500);
  void armEntryWhiteSeed();  // white absolute → then first page (anim from white)

  // Deferred nested-menu teardown (requestExitSubActivity + apply after pump).
  bool deferredMenuApply_ = false;
  uint8_t deferredMenuOrientation_ = 0;
  bool deferredMenuNeedRebuild_ = false;
  std::function<void()> deferredChildTransition_;
  // Chapter picker selected while state lock was busy (never block forever).
  bool hasDeferredChapterSwitch_ = false;
  int deferredChapterSwitch_ = 0;

  bool loadPluginTidx();
  void savePluginTidx() const;
  void requestPluginClose();
  void applyPendingRestoreIfReady();  // under renderingMutex
  int pageIndexForByteLocked(size_t byteOffset) const;
  void applyDeferredMenuClose();
  std::string displayTitle() const;

  // State lock helpers (non-recursive). UI may block waiting for display/index.
  bool lockState(TickType_t ticks = portMAX_DELAY) const;
  void unlockState() const;
  // Coherent page turn under lock (plugin and library share pageOffsets).
  void pageTurnLocked(int delta);
};
