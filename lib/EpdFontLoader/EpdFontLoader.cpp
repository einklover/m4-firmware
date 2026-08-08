#include "EpdFontLoader.h"

#include <HardwareSerial.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/CrossPointSettings.h"
#include "../../src/fontIds.h"
#include "../../src/managers/FontManager.h"
#include "../../src/util/M4FontPolicy.h"

std::vector<int> EpdFontLoader::loadedCustomIds;
M4FontPolicy::LoadResult EpdFontLoader::lastCanonicalResult = M4FontPolicy::LoadResult::NotAttempted;
bool EpdFontLoader::sdFontsLoaded_ = false;

namespace {
int hashFontId(const char* familyName, int size) {
  std::string key = std::string(familyName) + "-" + std::to_string(size);
  uint32_t hash = 5381;
  for (char c : key) hash = ((hash << 5) + hash) + static_cast<uint8_t>(c);
  return static_cast<int>(hash);
}

int sizeForEnum(uint8_t fontSizeEnum) {
  switch (fontSizeEnum) {
    case CrossPointSettings::SMALL:
      return 12;
    case CrossPointSettings::MEDIUM:
      return 14;
    case CrossPointSettings::LARGE:
      return 16;
    case CrossPointSettings::EXTRA_LARGE:
      return 18;
    default:
      return 16;
  }
}

bool isRuntimeTtfFamily(const std::string& familyName) {
  if (familyName.size() < 4) return false;
  std::string suffix = familyName.substr(familyName.size() - 4);
  std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return suffix == ".ttf";
}

// Runtime TTFs are expensive faces (stream + cmap + cache metadata). Reuse the
// exact current reader face across settings/layout reloads instead of recreating
// it whenever orientation or a non-font setting invalidates pagination.
std::string activeRuntimeTtfFamily;
int activeRuntimeTtfSize = -1;

bool insertCustomFamily(GfxRenderer& renderer, const char* familyName, int size) {
  EpdFontFamily* family = FontManager::getInstance().getCustomFontFamily(familyName, size);
  if (!family) {
    Serial.printf("[FontLoader] Failed to load '%s' size %d\n", familyName, size);
    return false;
  }
  const int id = hashFontId(familyName, size);
  renderer.insertFont(id, *family);
  Serial.printf("[FontLoader] Inserted custom font '%s' size %d id=%d\n", familyName, size, id);
  return true;
}

int loadAndInsertCustom(GfxRenderer& renderer, const char* familyName, int size, std::vector<int>& outIds) {
  if (!insertCustomFamily(renderer, familyName, size)) return -1;
  const int id = hashFontId(familyName, size);
  outIds.push_back(id);
  return id;
}

void promoteToReaderIds(GfxRenderer& renderer, const char* familyName, int size) {
  EpdFontFamily* family = FontManager::getInstance().getCustomFontFamily(familyName, size);
  if (!family) return;
  // The release epdfont is a single fixed ~16 px face. Promote it only to
  // reader/content IDs. Replacing UI_10/UI_12/SMALL with the same large face
  // makes system menus overlap because those layouts expect compact metrics.
  renderer.replaceFont(NOTOSANS_12_FONT_ID, *family);
  renderer.replaceFont(NOTOSANS_14_FONT_ID, *family);
  renderer.replaceFont(NOTOSANS_16_FONT_ID, *family);
  renderer.replaceFont(NOTOSANS_18_FONT_ID, *family);
  Serial.printf("[M4-FONT] Promoted NOTOSANS reader/content IDs to canonical SD epdfont '%s'; "
                "kept UI_10/UI_12/SMALL on compact builtin subset "
                "(fixed generated pixel size ~%dpt)\n",
                familyName, M4FontPolicy::kCanonicalEpdfontPixelSize);
}
}  // namespace

void EpdFontLoader::ensureFontsFromSd(GfxRenderer& renderer) {
  if (sdFontsLoaded_) {
    return;
  }
  Serial.printf("[M4-FONT] ensureFontsFromSd: first load this session\n");
  loadFontsFromSd(renderer);
}

void EpdFontLoader::loadFontsFromSd(GfxRenderer& renderer) {
  const std::vector<int> previousCustomIds = loadedCustomIds;
  loadedCustomIds.clear();
  lastCanonicalResult = M4FontPolicy::LoadResult::NotAttempted;
  FontManager::getInstance().invalidateScan();
  const auto& families = FontManager::getInstance().getAvailableFamilies();

#ifdef CROSSPOINT_MURPHY_M4
  M4FontPolicy::Inputs in;
  in.availableFamilies = families;
  in.hasCanonical = std::find(families.begin(), families.end(), M4FontPolicy::kCanonicalFamily) != families.end();
  in.mode = (SETTINGS.fontFamily == CrossPointSettings::FONT_CUSTOM) ? M4FontPolicy::FamilyMode::Custom
                                                                     : M4FontPolicy::FamilyMode::System;
  in.customFamily = SETTINGS.customFontFamily;

  const M4FontPolicy::Decision d = M4FontPolicy::decide(in);
  if (d.mutateSettingsToCustom) {
    SETTINGS.fontFamily = CrossPointSettings::FONT_CUSTOM;
    SETTINGS.saveToFile();
  }

  if (!d.diagnostic.empty()) {
    Serial.printf("[M4-FONT] DIAG: %s\n", d.diagnostic.c_str());
  }

  if (!in.hasCanonical) {
    lastCanonicalResult = M4FontPolicy::LoadResult::Missing;
  }

  const bool runtimeTtf = !d.loadCustomFamily.empty() && isRuntimeTtfFamily(d.loadCustomFamily);
  int runtimeReaderSize = -1;
  bool reuseRuntimeTtf = false;
  if (runtimeTtf) {
    runtimeReaderSize = SETTINGS.customFontSize == 0
                            ? sizeForEnum(SETTINGS.fontSize)
                            : std::max<int>(12, std::min<int>(48, SETTINGS.customFontSize));
    const int runtimeId = hashFontId(d.loadCustomFamily.c_str(), runtimeReaderSize);
    reuseRuntimeTtf = activeRuntimeTtfFamily == d.loadCustomFamily && activeRuntimeTtfSize == runtimeReaderSize &&
                      renderer.hasFont(runtimeId);
  }

  if (!reuseRuntimeTtf) {
    // Old custom mappings are value-copies of families containing raw EpdFont
    // pointers. Drop the renderer aliases before FontManager forgets/reloads
    // them so a newly selected family/size cannot be shadowed by insert-only IDs.
    for (int id : previousCustomIds) renderer.removeFont(id);
    FontManager::getInstance().clearLoadedFonts();
    if (!runtimeTtf) {
      activeRuntimeTtfFamily.clear();
      activeRuntimeTtfSize = -1;
    }
  } else {
    Serial.printf("[M4-FONT] Reusing runtime TTF face '%s' @%dpx (UI scales this same cache)\n",
                  d.loadCustomFamily.c_str(), runtimeReaderSize);
  }

  // 1) Explicit CUSTOM family → reader hash IDs. Runtime TTF is deliberately
  // ONE rasterizer face: the reader size. UI/status chrome resolves this same
  // face and uses GfxRenderer's bitmap scaling, so 12/14/16/18/20/24 no longer
  // allocate independent cmap/cache/mutex/stream state.
  if (!d.loadCustomFamily.empty()) {
    std::vector<int> sizes;
    if (runtimeTtf) {
      sizes.push_back(runtimeReaderSize);
    } else {
      // Preserve legacy epdfont behavior; its fixed bitmap artifact is cheap
      // compared with the runtime TTF rasterizer and existing IDs depend on it.
      sizes = {12, 14, 16, 18, 20, 24};
      const uint8_t explicitSize = SETTINGS.customFontSize == 0
                                       ? 0
                                       : std::max<uint8_t>(12, std::min<uint8_t>(48, SETTINGS.customFontSize));
      if (explicitSize != 0 && std::find(sizes.begin(), sizes.end(), explicitSize) == sizes.end()) {
        sizes.push_back(explicitSize);
      }
    }

    bool any = false;
    for (int sz : sizes) {
      any = (loadAndInsertCustom(renderer, d.loadCustomFamily.c_str(), sz, loadedCustomIds) >= 0) || any;
    }
    if (!any) {
      Serial.printf("[M4-FONT] DIAG: failed to load explicit custom '%s'\n", d.loadCustomFamily.c_str());
      if (runtimeTtf) {
        activeRuntimeTtfFamily.clear();
        activeRuntimeTtfSize = -1;
      }
    } else if (runtimeTtf) {
      activeRuntimeTtfFamily = d.loadCustomFamily;
      activeRuntimeTtfSize = runtimeReaderSize;
      Serial.printf("[M4-FONT] Runtime TTF single-face mode '%s' reader=%dpx; UI/status reuse + scale\n",
                    d.loadCustomFamily.c_str(), runtimeReaderSize);
    } else {
      Serial.printf("[M4-FONT] Loaded explicit CUSTOM family '%s' for reader\n", d.loadCustomFamily.c_str());
    }
  }

  // 2) System/UI promotion: canonical only (never families.front() / Latin-only).
  if (!d.promoteSystemFamily.empty()) {
    EpdFontFamily* fam = FontManager::getInstance().getCustomFontFamily(
        d.promoteSystemFamily, M4FontPolicy::kCanonicalEpdfontPixelSize);
    if (!fam) {
      Serial.printf("[M4-FONT] DIAG: failed to load canonical '%s' for system promotion\n",
                    d.promoteSystemFamily.c_str());
      lastCanonicalResult = M4FontPolicy::LoadResult::LoadFailed;
    } else {
      promoteToReaderIds(renderer, d.promoteSystemFamily.c_str(), M4FontPolicy::kCanonicalEpdfontPixelSize);
      lastCanonicalResult = M4FontPolicy::LoadResult::Promoted;
    }
  } else if (in.mode == M4FontPolicy::FamilyMode::System) {
    if (lastCanonicalResult == M4FontPolicy::LoadResult::NotAttempted) {
      lastCanonicalResult = M4FontPolicy::LoadResult::Missing;
    }
    Serial.printf("[M4-FONT] DIAG: copy %s to SD for full reader/app CJK; compact UI subset otherwise. "
                  "Other epdfonts are never auto-promoted.\n",
                  M4FontPolicy::kCanonicalSdPath);
  }
  sdFontsLoaded_ = true;
  return;
#endif

  // Non-M4: original behavior — reload only the selected custom face.
  for (int id : previousCustomIds) renderer.removeFont(id);
  FontManager::getInstance().clearLoadedFonts();
  if (SETTINGS.fontFamily == CrossPointSettings::FONT_CUSTOM) {
    if (strlen(SETTINGS.customFontFamily) > 0) {
      Serial.printf("Loading custom font: %s size %d\n", SETTINGS.customFontFamily, SETTINGS.fontSize);
      Serial.flush();
      int size = sizeForEnum(SETTINGS.fontSize);
      if (SETTINGS.customFontSize != 0) size = SETTINGS.customFontSize;
      loadAndInsertCustom(renderer, SETTINGS.customFontFamily, size, loadedCustomIds);
    }
  }
  sdFontsLoaded_ = true;
}

int EpdFontLoader::getBestFontId(const char* familyName, int size) {
  if (!familyName || strlen(familyName) == 0) return -1;

  const int id = hashFontId(familyName, size);
  for (int loadedId : loadedCustomIds) {
    if (loadedId == id) return id;
  }
  return -1;
}
