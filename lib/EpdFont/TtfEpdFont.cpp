#include "TtfEpdFont.h"

#include <HardwareSerial.h>

#include <algorithm>
#include <cstring>

namespace {

// FsFile-backed seekable stream for the parser.
class SdTtfStream : public ttf::TtfStream {
 public:
  bool open(const String& path) {
    close();
    return SdMan.openFileForRead("TtfFont", path.c_str(), file_);
  }
  void close() {
    if (file_.isOpen()) file_.close();
  }
  bool isOpen() const { return file_.isOpen(); }

  uint32_t size() const override { return file_.isOpen() ? file_.fileSize() : 0; }
  bool seek(uint32_t pos) override { return file_.isOpen() && file_.seekSet(pos); }
  uint32_t read(void* dst, uint32_t n) override {
    if (!file_.isOpen()) return 0;
    return file_.read(dst, n);
  }

 private:
  FsFile file_;
};

void* ttfAlloc(size_t n) {
#if defined(ESP32) && defined(BOARD_HAS_PSRAM)
  if (psramFound()) return ps_malloc(n);
#endif
  return malloc(n);
}

void ttfFree(void* p) { free(p); }

}  // namespace

void* TtfEpdFont::ttfAlloc(size_t n) { return ::ttfAlloc(n); }
void TtfEpdFont::ttfFree(void* p) { ::ttfFree(p); }

TtfEpdFont::TtfEpdFont(const String& path, uint16_t sizePx) : EpdFont(&data_), path_(path), sizePx_(sizePx) {
#if defined(ESP32)
  mutex_ = xSemaphoreCreateMutex();
#endif
  for (int i = 0; i < kMaxSlots; i++) {
    entries_[i].cp = 0xFFFFFFFF;
  }

  auto* s = new SdTtfStream();
  stream_ = s;
  if (!s->open(path_)) {
    Serial.printf("[TTF] Failed to open %s\n", path_.c_str());
    delete s;
    stream_ = nullptr;
    return;
  }
  if (!font_.init(*s)) {
    Serial.printf("[TTF] Invalid TTF %s: %s\n", path_.c_str(), font_.lastError());
    return;
  }

  // Scale metrics once; the font file stays open for per-glyph glyf reads.
  const float scale = (float)sizePx_ / (float)font_.unitsPerEm();
  int32_t asc = 0, desc = 0, gap = 0;
  font_.fontVMetrics(asc, desc, gap);
  // GfxRenderer positions glyphs as baseline - glyph.top.  hhea.ascender is
  // only a typographic metric and can be smaller than the actual CJK outline;
  // use head.yMax as the same actual-top correction used by fontconvert.py.
  const int actualTopPx = (int)std::lround(font_.fontBBoxYMax() * scale);
  // The renderer's y coordinate is the top of the line box.  Use the font
  // metric directly; the previous extra pixel made the correction too strong.
  const int ascPx = std::max((int)std::lround(asc * scale), actualTopPx);
  const int descPx = (int)std::lround(desc * scale);
  int linePx = (int)std::lround((asc - desc + gap) * scale);
  if (linePx < 1) linePx = sizePx_;
  if (linePx > 255) linePx = 255;

  data_.bitmap = nullptr;
  data_.glyph = nullptr;
  data_.intervals = nullptr;
  data_.intervalCount = 0;
  data_.advanceY = (uint8_t)linePx;
  data_.ascender = ascPx;
  data_.descender = descPx;
  data_.is2Bit = true;

  valid_ = true;
  Serial.printf("[TTF] Loaded %s @%upx (unitsPerEm=%u lineH=%u asc=%d desc=%d)\n", path_.c_str(), sizePx_,
                font_.unitsPerEm(), data_.advanceY, data_.ascender, data_.descender);
}

TtfEpdFont::~TtfEpdFont() {
  clearCaches();
  delete stream_;
  stream_ = nullptr;
#if defined(ESP32)
  if (mutex_) {
    vSemaphoreDelete(mutex_);
    mutex_ = nullptr;
  }
#endif
}

void TtfEpdFont::evictSlot(int slot) const {
  if (slot < 0 || slot >= kMaxSlots) return;
  if (entries_[slot].bitmap) {
    ttfFree(entries_[slot].bitmap);
    cacheBytes_ -= entries_[slot].bitmapSize;
  }
  entries_[slot] = Entry{};
}

void TtfEpdFont::clearCaches() {
#if defined(ESP32)
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
#endif
  for (int i = 0; i < kMaxSlots; i++) {
    evictSlot(i);
  }
  font_.clearScratch();
#if defined(ESP32)
  if (mutex_) xSemaphoreGive(mutex_);
#endif
}

int TtfEpdFont::ensureGlyph(uint32_t cp) const {
  if (!valid_) return -1;

  // Hit.
  for (int i = 0; i < kMaxSlots; i++) {
    if (entries_[i].cp == cp) {
      entries_[i].lastAccess = ++accessCounter_;
      return i;
    }
  }

  uint16_t gid = 0;
  if (!font_.findGlyph(cp, gid)) return -1;
  if (gid == 0 && cp != '?') {
    // Missing codepoint: share the '?' entry (renders '?' like the epdfont
    // backend, and keeps GfxRenderer::hasTextGlyphs' "same as '?'" detection
    // working so UI chrome falls back to the built-in CJK face).
    return ensureGlyph('?');
  }
  ttf::GlyphBitmap gb;
  if (!font_.rasterize(gid, sizePx_, gb)) {
    // Unsupported glyph (e.g. compound with point args): tofu via .notdef.
    if (gid != 0 && !font_.rasterize(0, sizePx_, gb)) return -1;
  }

  // Slot: free one or LRU.
  int slot = -1;
  uint32_t minAccess = 0xFFFFFFFF;
  for (int i = 0; i < kMaxSlots; i++) {
    if (entries_[i].cp == 0xFFFFFFFF) {
      slot = i;
      break;
    }
    if (entries_[i].lastAccess < minAccess) {
      minAccess = entries_[i].lastAccess;
      slot = i;
    }
  }
  evictSlot(slot);

  // Bitmap: allocate from PSRAM (preferred) / heap, bounded by budget.
  uint8_t* bmp = nullptr;
  const uint32_t packedLen = gb.packedLen;
  if (packedLen > 0 && gb.data) {
    bmp = (uint8_t*)ttfAlloc(packedLen);
    if (!bmp) {
      Serial.printf("[TTF] glyph alloc %u failed (OOM)\n", packedLen);
      bmp = nullptr;
    } else {
      std::memcpy(bmp, gb.data, packedLen);
    }
  }

  entries_[slot].cp = cp;
  entries_[slot].lastAccess = ++accessCounter_;
  entries_[slot].glyph.width = (uint8_t)gb.width;
  entries_[slot].glyph.height = (uint8_t)gb.height;
  entries_[slot].glyph.advanceX = (uint8_t)std::max(0, std::min(255, (int)gb.advance));
  entries_[slot].glyph.left = gb.xoff;
  entries_[slot].glyph.top = gb.yoff;
  entries_[slot].glyph.dataLength = packedLen;
  entries_[slot].glyph.dataOffset = cp;  // reuses EpdGlyph as the cache key
  entries_[slot].bitmap = bmp;
  entries_[slot].bitmapSize = packedLen;
  cacheBytes_ += packedLen;

  // Bounded budget: evict LRU (sparing the fresh slot) until under budget.
  if (cacheBytes_ > kCacheBudget) {
    for (int pass = 0; pass < kMaxSlots && cacheBytes_ > kCacheBudget; pass++) {
      int victim = -1;
      uint32_t la = 0xFFFFFFFF;
      for (int i = 0; i < kMaxSlots; i++) {
        if (i == slot || entries_[i].cp == 0xFFFFFFFF) continue;
        if (entries_[i].lastAccess < la) {
          la = entries_[i].lastAccess;
          victim = i;
        }
      }
      if (victim < 0) break;
      evictSlot(victim);
    }
  }

  return slot;
}

const EpdGlyph* TtfEpdFont::getGlyph(uint32_t cp, const EpdFontStyles::Style style) const {
  (void)style;
#if defined(ESP32)
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
#endif
  const int slot = ensureGlyph(cp);
#if defined(ESP32)
  if (mutex_) xSemaphoreGive(mutex_);
#endif
  if (slot < 0) return nullptr;
  return &entries_[slot].glyph;
}

const uint8_t* TtfEpdFont::loadGlyphBitmap(const EpdGlyph* glyph, uint8_t* buffer,
                                           const EpdFontStyles::Style style) const {
  (void)style;
  if (!glyph) return nullptr;
  const uint32_t cp = glyph->dataOffset;
#if defined(ESP32)
  if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
#endif
  const int slot = ensureGlyph(cp);
  const uint8_t* result = nullptr;
  if (slot >= 0 && entries_[slot].bitmap && entries_[slot].bitmapSize > 0) {
    if (buffer) {
      std::memcpy(buffer, entries_[slot].bitmap, entries_[slot].bitmapSize);
      result = buffer;
    } else {
      result = entries_[slot].bitmap;
    }
  }
#if defined(ESP32)
  if (mutex_) xSemaphoreGive(mutex_);
#endif
  return result;
}
