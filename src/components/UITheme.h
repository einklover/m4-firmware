#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"
#include "util/M4TouchNavigation.h"

enum class ThemeType { Lyra, Fengyan };

class UITheme {
  // Static instance
  static UITheme instance;

 public:
  // Thin facade keeps every existing GUI.draw* call source-compatible while
  // centralizing M4 navigation chrome. Theme-specific rendering is still
  // delegated to Fengyan/Lyra; only headers/button-hint footers are augmented.
  class Facade {
   public:
    explicit Facade(const UITheme& owner) : owner_(owner) {}

    void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) const {
      owner_.getTheme().drawProgressBar(renderer, rect, current, total);
    }
    void drawBattery(const GfxRenderer& renderer, Rect rect, bool showPercentage = true) const {
      owner_.getTheme().drawBattery(renderer, rect, showPercentage);
    }
    void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                         const char* btn4, bool force = false) const {
#if defined(CROSSPOINT_MURPHY_M4)
      if (M4TouchNavigation::enabled()) {
        M4TouchNavigation::drawBottomBar(renderer);
        return;
      }
#endif
      owner_.getTheme().drawButtonHints(renderer, btn1, btn2, btn3, btn4, force);
    }
    void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn,
                             bool force = false) const {
      owner_.getTheme().drawSideButtonHints(renderer, topBtn, bottomBtn, force);
    }
    void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                  const std::function<std::string(int index)>& rowTitle,
                  const std::function<std::string(int index)>& rowSubtitle,
                  const std::function<UIIcon(int index)>& rowIcon,
                  const std::function<std::string(int index)>& rowValue) const {
      owner_.getTheme().drawList(renderer, rect, itemCount, selectedIndex, rowTitle, rowSubtitle, rowIcon, rowValue);
    }
    void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title) const {
      owner_.getTheme().drawHeader(renderer, rect, title);
      M4TouchNavigation::drawHeaderBack(renderer, rect);
    }
    void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs, bool selected) const {
      owner_.getTheme().drawTabBar(renderer, rect, tabs, selected);
    }
    void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                             int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                             std::function<bool()> storeCoverBuffer) const {
      owner_.getTheme().drawRecentBookCover(renderer, rect, recentBooks, selectorIndex, coverRendered,
                                             coverBufferStored, bufferRestored, std::move(storeCoverBuffer));
    }
    void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                        const std::function<std::string(int index)>& buttonLabel,
                        const std::function<UIIcon(int index)>& rowIcon) const {
      owner_.getTheme().drawButtonMenu(renderer, rect, buttonCount, selectedIndex, buttonLabel, rowIcon);
    }
    Rect drawPopup(const GfxRenderer& renderer, const char* message) const {
      return owner_.getTheme().drawPopup(renderer, message);
    }
    void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, int progress) const {
      owner_.getTheme().fillPopupProgress(renderer, layout, progress);
    }
    void drawReadingProgressBar(const GfxRenderer& renderer, size_t bookProgress) const {
      owner_.getTheme().drawReadingProgressBar(renderer, bookProgress);
    }

   private:
    const UITheme& owner_;
  };

  UITheme();
  static UITheme& getInstance() { return instance; }

  const ThemeMetrics& getMetrics() { return *currentMetrics; }
  const BaseTheme& getTheme() const { return *currentTheme; }
  const Facade& getFacade() const { return facade; }
  ThemeType getThemeType() { return currentThemeType; }
  void reload();
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverWidth, int coverHeight);
  static UIIcon getFileIcon(const std::string& filename);

 private:
  const ThemeMetrics* currentMetrics = nullptr;
  // Owns the active theme; reload() must replace without leaking the prior instance.
  std::unique_ptr<BaseTheme> currentTheme;
  ThemeType currentThemeType = ThemeType::Fengyan;
  Facade facade{*this};
};

// Helper macro to access current theme through the M4 navigation facade.
#define GUI UITheme::getInstance().getFacade()
