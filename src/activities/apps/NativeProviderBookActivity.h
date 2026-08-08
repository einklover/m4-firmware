#pragma once

#include "../ActivityWithSubactivity.h"

#include <functional>
#include <memory>
#include <string>

namespace M4PluginTocList {
class PagedTitleSource;
}

class NativeProviderBookActivity final : public ActivityWithSubactivity {
 public:
  NativeProviderBookActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             std::string providerId, std::string bookId,
                             std::string appId, std::string title,
                             const std::function<void()>& onExitBook);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  std::string debugUiJson() override;

 private:
  enum class State { OpeningToc, Toc, Loading, Login, Reader, Error };

  bool prepareCatalog();
  void openToc();
  void requestChapter(int index0);
  void openLogin();
  bool openReadyReader(int index0);
  std::string titleAt(int index0) const;
  void renderLoading(bool force = false);
  void renderError();

  std::string providerId_;
  std::string bookId_;
  std::string appId_;
  std::string title_;
  std::string appDataRoot_;
  std::string error_;
  std::function<void()> onExitBook_;
  std::shared_ptr<M4PluginTocList::PagedTitleSource> titles_;
  int chapterCount_ = 0;
  int currentIndex_ = 0;
  int loadingIndex_ = -1;
  State state_ = State::OpeningToc;
  bool tocBackPending_ = false;
  bool tocSelectionPending_ = false;
  int tocSelectedIndex_ = -1;
  bool readerBackPending_ = false;
  bool loginFinishedPending_ = false;
  bool loginSucceeded_ = false;
  uint32_t lastLoadingPaintMs_ = 0;
  std::string lastLoadingSignature_;
};
