#pragma once

#include <map>
#include <string>
#include <vector>

#include "EpdFontFamily.h"

class FontManager {
 public:
  static FontManager& getInstance();

  // Scan SD card for fonts
  void scanFonts();

  // Get list of available font family names
  const std::vector<std::string>& getAvailableFamilies();

  // Load a specific family and size (returns pointer to cached family or new one)
  EpdFontFamily* getCustomFontFamily(const std::string& familyName, int fontSize);

  // 清除已加载字体的内存缓存（切换字体时调用，迫使重新加载并写入 flash）
  void clearLoadedFonts();

  // Force next getAvailableFamilies() to re-scan /fonts (M4 hot-plug / first boot).
  void invalidateScan() { scanned = false; availableFamilies.clear(); }

 private:
  FontManager() = default;
  ~FontManager();

  std::vector<std::string> availableFamilies;
  bool scanned = false;

  // Map: FamilyName -> Size -> EpdFontFamily*
  std::map<std::string, std::map<int, EpdFontFamily*>> loadedFonts;
};
