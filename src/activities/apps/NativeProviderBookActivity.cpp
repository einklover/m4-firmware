#include "NativeProviderBookActivity.h"

#include "activities/reader/TxtReaderActivity.h"
#include "activities/reader/TxtReaderChapterSelectionActivity.h"
#include "apps/M4ContentProviderSession.h"
#include "apps/M4PluginReaderSession.h"
#include "apps/providers/M4NativeProviderManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/M4PluginReaderBridge.h"
#include "util/M4PluginTocList.h"
#include "util/M4UiText.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <SDCardManager.h>
#include <Txt.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

NativeProviderBookActivity::NativeProviderBookActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput, std::string providerId,
    std::string bookId, std::string appId, std::string title,
    const std::function<void()>& onExitBook)
    : ActivityWithSubactivity("NativeProviderBook", renderer, mappedInput),
      providerId_(std::move(providerId)),
      bookId_(std::move(bookId)),
      appId_(std::move(appId)),
      title_(std::move(title)),
      onExitBook_(onExitBook) {}

void NativeProviderBookActivity::onEnter() {
  ActivityWithSubactivity::onEnter();
  state_ = State::OpeningToc;
  error_.clear();
  if (!prepareCatalog()) {
    state_ = State::Error;
    renderError();
    return;
  }
  openToc();
}

void NativeProviderBookActivity::onExit() {
  ActivityWithSubactivity::onExit();
  titles_.reset();
}

bool NativeProviderBookActivity::prepareCatalog() {
  if (!M4NativeProviderManager::ensureBook(providerId_, bookId_, appId_, title_)) {
    error_ = "无法读取书籍目录";
    return false;
  }
  appDataRoot_ = M4NativeProviderManager::appDataRootFor(providerId_, bookId_);
  M4ContentProvider::ChapterCatalogSpec catalog;
  if (!M4ContentProviderSession::catalogFor(providerId_, bookId_, 0, catalog) || catalog.fileRelPath.empty()) {
    error_ = "目录尚未准备好";
    return false;
  }
  std::string abs;
  if (M4PluginReaderBridge::resolveUnderDataRoot(appDataRoot_, catalog.fileRelPath.c_str(), abs) !=
      M4PluginReaderBridge::OpenError::Ok) {
    error_ = "目录路径无效";
    return false;
  }
  titles_ = M4PluginTocList::openPagedFileRows(abs, catalog);
  if (!titles_ || titles_->rowCount() == 0) {
    error_ = "目录为空";
    return false;
  }
  chapterCount_ = static_cast<int>(titles_->rowCount());
  if (currentIndex_ < 0) currentIndex_ = 0;
  if (currentIndex_ >= chapterCount_) currentIndex_ = chapterCount_ - 1;
  return true;
}

void NativeProviderBookActivity::openToc() {
  if (!titles_ && !prepareCatalog()) {
    state_ = State::Error;
    renderError();
    return;
  }
  tocBackPending_ = false;
  tocSelectionPending_ = false;
  tocSelectedIndex_ = -1;
  state_ = State::Toc;
  auto source = titles_;
  auto loader = [source](int first, int count, std::vector<std::string>& pageTitles,
                         std::vector<uint8_t>& pagePresent) {
    return source->loadPage(first, count, pageTitles, pagePresent);
  };
  enterNewActivity(new TxtReaderChapterSelectionActivity(
      renderer, mappedInput, chapterCount_, std::move(loader), currentIndex_,
      [this]() {
        tocBackPending_ = true;
        requestExitSubActivity();
      },
      [this](int index0) {
        tocSelectedIndex_ = index0;
        tocSelectionPending_ = true;
        requestExitSubActivity();
      },
      title_.empty() ? std::string("目  录") : title_));
}

std::string NativeProviderBookActivity::titleAt(int index0) const {
  if (!titles_ || index0 < 0) return {};
  std::vector<std::string> t;
  std::vector<uint8_t> p;
  if (!titles_->loadPage(index0, 1, t, p) || t.empty() || p.empty() || !p[0]) return {};
  return t[0];
}

void NativeProviderBookActivity::requestChapter(int index0) {
  if (index0 < 0 || index0 >= chapterCount_) return;
  currentIndex_ = index0;
  loadingIndex_ = index0;
  error_.clear();
  state_ = State::Loading;
  lastLoadingSignature_.clear();
  lastLoadingPaintMs_ = 0;
  if (!M4NativeProviderManager::ensureChapter(providerId_, bookId_, index0, true)) {
    const auto st = M4ContentProviderSession::chapterAt(providerId_, bookId_, index0);
    if (st.state != M4ContentProvider::ChapterReady::Ready) {
      error_ = "无法启动章节加载";
      state_ = State::Error;
      renderError();
      return;
    }
  }
  renderLoading(true);
}

bool NativeProviderBookActivity::openReadyReader(int index0) {
  const auto st = M4ContentProviderSession::chapterAt(providerId_, bookId_, index0);
  if (st.state != M4ContentProvider::ChapterReady::Ready || st.cacheRelPath.empty()) return false;
  std::string abs;
  if (M4PluginReaderBridge::resolveUnderDataRoot(appDataRoot_, st.cacheRelPath.c_str(), abs) !=
      M4PluginReaderBridge::OpenError::Ok) {
    error_ = "章节缓存路径无效";
    return false;
  }
  auto txt = std::make_unique<Txt>(abs, "/.crosspoint");
  if (!txt->load() || !txt->isEncodingSupported() || txt->getFileSize() == 0) {
    error_ = "章节缓存不可读取";
    return false;
  }

  TxtReaderActivity::PluginSession sess;
  sess.active = true;
  sess.suppressRecentBooks = false;
  sess.suppressOpenEpubPath = true;
  sess.progressiveIndex = true;
  sess.bookId = bookId_;
  sess.chapterUid = st.chapterUid;
  sess.chapterIndex = index0;
  sess.providerId = providerId_;
  sess.appId = appId_;
  sess.appDataRoot = appDataRoot_;
  sess.cacheRelPath = st.cacheRelPath;
  sess.progressKey = providerId_ + ":" + bookId_ + ":" + st.chapterUid;
  sess.titleOverride = titleAt(index0);
  sess.generation = M4PluginReaderSession::bumpGeneration();
  sess.providerManaged = true;

  readerBackPending_ = false;
  state_ = State::Reader;
  auto onReaderClose = [this]() {
    int requestedIndex = -1;
    if (subActivity) {
      auto* r = static_cast<TxtReaderActivity*>(subActivity.get());
      const auto p = r->pluginProgressSnapshot();
      if (p.valid && p.switchChapterIndex >= 0) requestedIndex = p.switchChapterIndex;
    }
    if (requestedIndex >= 0) {
      tocSelectedIndex_ = requestedIndex;
      tocSelectionPending_ = true;
    }
    readerBackPending_ = true;
    requestExitSubActivity();
  };
  enterNewActivity(new TxtReaderActivity(renderer, mappedInput, std::move(txt), onReaderClose, onReaderClose,
                                         std::move(sess)));
  return true;
}

void NativeProviderBookActivity::renderLoading(bool force) {
  const uint32_t now = millis();
  auto p = M4NativeProviderManager::progress();
  const auto st = M4ContentProviderSession::chapterAt(providerId_, bookId_, loadingIndex_);
  std::string phase = "准备章节";
  size_t received = 0;
  size_t written = 0;
  int pct = st.pct;
  if (p.providerId == providerId_ && p.bookId == bookId_ && p.chapterIndex0 == loadingIndex_) {
    received = p.receivedBytes;
    written = p.writtenBytes;
    pct = p.percent;
    switch (p.phase) {
      case M4NativeProvider::Phase::Resolving: phase = "解析章节"; break;
      case M4NativeProvider::Phase::Connecting: phase = "连接服务器"; break;
      case M4NativeProvider::Phase::Receiving: phase = "接收正文"; break;
      case M4NativeProvider::Phase::Decoding: phase = "处理正文"; break;
      case M4NativeProvider::Phase::Writing: phase = "写入缓存"; break;
      case M4NativeProvider::Phase::AuthRequired: phase = "需要重新登录"; break;
      case M4NativeProvider::Phase::Error: phase = "加载失败"; break;
      case M4NativeProvider::Phase::Cancelled: phase = "已取消"; break;
      case M4NativeProvider::Phase::Ready: phase = "准备打开"; break;
      default: break;
    }
  }
  const uint32_t started = p.startedMs ? p.startedMs : now;
  const uint32_t elapsed = (now - started) / 1000u;
  char detail[128];
  if (received || written) {
    std::snprintf(detail, sizeof(detail), "已接收 %u KB · 已写入 %u KB · %us",
                  static_cast<unsigned>(received / 1024u), static_cast<unsigned>(written / 1024u),
                  static_cast<unsigned>(elapsed));
  } else {
    std::snprintf(detail, sizeof(detail), "已用时 %u 秒", static_cast<unsigned>(elapsed));
  }
  const std::string sig = phase + "|" + detail + "|" + std::to_string(pct) + "|" + std::to_string(st.state == M4ContentProvider::ChapterReady::Error);
  if (!force && sig == lastLoadingSignature_ && now - lastLoadingPaintMs_ < 1000) return;
  lastLoadingSignature_ = sig;
  lastLoadingPaintMs_ = now;

  renderer.clearScreen();
  const auto metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 title_.empty() ? "在线阅读" : title_.c_str());
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 180, phase.c_str(), true, EpdFontFamily::BOLD);
  const std::string chapterTitle = titleAt(loadingIndex_);
  if (!chapterTitle.empty()) M4UiText::drawCentered(renderer, UI_10_FONT_ID, 230, chapterTitle.c_str());
  if (pct > 0) {
    GUI.drawProgressBar(renderer, Rect{70, 285, renderer.getScreenWidth() - 140, 12},
                        static_cast<size_t>(std::min(100, pct)), 100);
  }
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, 330, detail);
  const auto labels = mappedInput.mapLabels("« 返回", st.state == M4ContentProvider::ChapterReady::Error ? "重试" : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void NativeProviderBookActivity::renderError() {
  renderer.clearScreen();
  const auto metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 title_.empty() ? "在线阅读" : title_.c_str());
  M4UiText::drawCentered(renderer, UI_12_FONT_ID, 190, "无法打开", true, EpdFontFamily::BOLD);
  M4UiText::drawCentered(renderer, UI_10_FONT_ID, 245, error_.c_str());
  const auto labels = mappedInput.mapLabels("« 返回", "重试", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void NativeProviderBookActivity::loop() {
  if (subActivity) {
    const bool closed = pumpSubActivityFrame();
    if (!closed) return;
    if (state_ == State::Toc) {
      if (tocBackPending_) {
        onExitBook_();
        return;
      }
      if (tocSelectionPending_ && tocSelectedIndex_ >= 0) {
        const int next = tocSelectedIndex_;
        tocSelectionPending_ = false;
        requestChapter(next);
        return;
      }
    } else if (state_ == State::Reader && readerBackPending_) {
      readerBackPending_ = false;
      if (tocSelectionPending_ && tocSelectedIndex_ >= 0) {
        const int next = tocSelectedIndex_;
        tocSelectionPending_ = false;
        requestChapter(next);
      } else {
        openToc();
      }
      return;
    }
  }

  if (state_ == State::Loading) {
    const auto st = M4ContentProviderSession::chapterAt(providerId_, bookId_, loadingIndex_);
    if (st.state == M4ContentProvider::ChapterReady::Ready) {
      if (!openReadyReader(loadingIndex_)) {
        state_ = State::Error;
        if (error_.empty()) error_ = "章节打开失败";
        renderError();
      }
      return;
    }
    renderLoading(false);
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
      M4NativeProviderManager::cancelForeground();
      openToc();
      return;
    }
    if (st.state == M4ContentProvider::ChapterReady::Error &&
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      requestChapter(loadingIndex_);
    }
    return;
  }

  if (state_ == State::Error) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasBackGesture()) {
      openToc();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (loadingIndex_ >= 0) requestChapter(loadingIndex_);
      else if (prepareCatalog()) openToc();
    }
  }
}

std::string NativeProviderBookActivity::debugUiJson() {
  return std::string("{\"kind\":\"native_provider_book\",\"provider\":\"") + providerId_ +
         "\",\"book\":\"" + bookId_ + "\",\"chapter\":" + std::to_string(currentIndex_) +
         ",\"state\":" + std::to_string(static_cast<int>(state_)) + "}";
}
