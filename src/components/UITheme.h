#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"

enum class ThemeType { Lyra, Fengyan };

class UITheme {
  // Static instance
  static UITheme instance;

 public:
  UITheme();
  static UITheme& getInstance() { return instance; }

  const ThemeMetrics& getMetrics() { return *currentMetrics; }
  const BaseTheme& getTheme() { return *currentTheme; }
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
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
