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

bool insertCustomFamily(GfxRenderer& renderer, const char* familyName, int size) {
  EpdFontFamily* family = FontManager::getInstance().getCustomFontFamily(familyName, size);
  if (!family) {
    Serial.printf("[FontLoader] Failed to load '%s' size %d from /fonts/%s.epdfont\n", familyName, size,
                  familyName);
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
  loadedCustomIds.clear();
  lastCanonicalResult = M4FontPolicy::LoadResult::NotAttempted;
  FontManager::getInstance().clearLoadedFonts();
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

  // 1) Explicit CUSTOM family → reader hash IDs only (no UI promotion unless canonical).
  if (!d.loadCustomFamily.empty()) {
    std::vector<int> sizes = {12, 14, 16, 18, 20, 24};
    // Runtime TTF: honor the exact customFontSize (12..48) so the reader's
    // getReaderFontId() customFontSize branch resolves to a loaded id.
    const uint8_t explicitSize = SETTINGS.customFontSize == 0
                                     ? 0
                                     : std::max<uint8_t>(12, std::min<uint8_t>(48, SETTINGS.customFontSize));
    if (explicitSize != 0 && std::find(sizes.begin(), sizes.end(), explicitSize) == sizes.end()) {
      sizes.push_back(explicitSize);
    }
    bool any = false;
    for (int sz : sizes) {
      any = (loadAndInsertCustom(renderer, d.loadCustomFamily.c_str(), sz, loadedCustomIds) >= 0) || any;
    }
    if (!any) {
      Serial.printf("[M4-FONT] DIAG: failed to load explicit custom '%s'\n", d.loadCustomFamily.c_str());
    } else {
      Serial.printf("[M4-FONT] Loaded explicit CUSTOM family '%s' for reader (not auto-promoted to UI "
                    "unless it is the canonical file)\n",
                    d.loadCustomFamily.c_str());
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

  // Non-M4: original behavior — load only when FONT_CUSTOM is set.
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
