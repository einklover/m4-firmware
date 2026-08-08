#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <SDCardManager.h>

#include "EpdFont.h"
#include "TtfReader.h"

// Runtime TrueType font backend for the existing EpdFont interface.
//
// Streams glyf outlines from the SD card on demand (see ttf::TtfFont); only the
// table directory + cmap + a bounded LRU glyph cache stay resident. Bitmaps are
// 2-bit grayscale (is2Bit=true) matching GfxRenderer's packing.
//
// getGlyph()/loadGlyphBitmap() are const but mutate the LRU cache; guarded by a
// mutex so the face can be shared across render tasks.
class TtfEpdFont : public EpdFont {
 public:
  TtfEpdFont(const String& path, uint16_t sizePx);
  ~TtfEpdFont() override;

  const EpdGlyph* getGlyph(uint32_t cp, const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override;
  const uint8_t* loadGlyphBitmap(const EpdGlyph* glyph, uint8_t* buffer,
                                 const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override;
  const EpdFontData* getData(const EpdFontStyles::Style style = EpdFontStyles::REGULAR) const override { return &data_; }

  bool isRuntimeTtf() const override { return true; }

  bool valid() const { return valid_; }
  const char* lastError() const { return font_.lastError(); }
  uint16_t sizePx() const { return sizePx_; }

  // Free cached bitmaps (called on font-switch to bound memory).
  void clearCaches();

 private:
  struct Entry {
    uint32_t cp = 0xFFFFFFFF;  // key; 0xFFFFFFFF = empty
    uint32_t lastAccess = 0;
    EpdGlyph glyph{};
    uint8_t* bitmap = nullptr;
    uint32_t bitmapSize = 0;
  };

  // Slot metadata is allocated from PSRAM at runtime rather than embedded in
  // the face object. This keeps the scarce internal heap close to epdfont's
  // model: RAM holds small handles/addresses while glyph data lives off-chip.
  static constexpr int kMaxSlots = 512;
  static constexpr size_t kCacheBudget = 768 * 1024;

  // Ensure a codepoint's glyph+bitmap is cached; returns slot index or -1.
  // Caller must hold mutex_.
  int ensureGlyph(uint32_t cp) const;
  void evictSlot(int slot) const;

  String path_;
  uint16_t sizePx_ = 0;
  bool valid_ = false;
  EpdFontData data_{};

  ttf::TtfStream* stream_ = nullptr;  // owned
  mutable ttf::TtfFont font_;

  mutable Entry* entries_ = nullptr;  // kMaxSlots, PSRAM-first
  mutable uint32_t accessCounter_ = 0;
  mutable size_t cacheBytes_ = 0;
#if defined(ESP32)
  mutable SemaphoreHandle_t mutex_ = nullptr;
#endif

  static void* ttfAlloc(size_t n);
  static void ttfFree(void* p);
};
