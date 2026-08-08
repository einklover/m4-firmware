#include "TtfEpdFont.h"

#include <HardwareSerial.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace {

// FsFile-backed seekable stream for normal user fonts on SD.
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

// Borrowed memory-backed stream for an embedded emergency/UI subset. ESP32-S3
// flash is memory mapped, so memcpy reads the const array without copying the
// font into heap/PSRAM. The array must outlive this stream (normally static).
class MemoryTtfStream : public ttf::TtfStream {
 public:
  MemoryTtfStream(const uint8_t* data, uint32_t size) : data_(data), size_(size) {}

  uint32_t size() const override { return size_; }
  bool seek(uint32_t pos) override {
    if (pos > size_) return false;
    pos_ = pos;
    return true;
  }
  uint32_t read(void* dst, uint32_t n) override {
    if (!data_ || !dst || pos_ >= size_) return 0;
    const uint32_t remaining = size_ - pos_;
    const uint32_t take = std::min(n, remaining);
    if (take > 0) {
      std::memcpy(dst, data_ + pos_, take);
      pos_ += take;
    }
    return take;
  }

 private:
  const uint8_t* data_ = nullptr;
  uint32_t size_ = 0;
  uint32_t pos_ = 0;
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

bool TtfEpdFont::allocateEntries() {
  if (maxSlots_ == 0) maxSlots_ = 1;
  if (cacheBudget_ == 0) cacheBudget_ = 1;

  // Keep lookup/LRU metadata off the scarce internal heap. Runtime reader
  // faces normally use 512 entries; a tiny embedded face can use ~96.
  entries_ = static_cast<Entry*>(ttfAlloc(sizeof(Entry) * maxSlots_));
  if (!entries_) {
    Serial.printf("[TTF] Failed to allocate glyph metadata slots=%u bytes=%u\n",
                  static_cast<unsigned>(maxSlots_),
                  static_cast<unsigned>(sizeof(Entry) * maxSlots_));
    return false;
  }
  for (uint16_t i = 0; i < maxSlots_; ++i) {
    new (&entries_[i]) Entry();
  }
  return true;
}

bool TtfEpdFont::finishInit(const char* sourceLabel) {
  if (!stream_) return false;
  if (!font_.init(*stream_)) {
    Serial.printf("[TTF] Invalid TTF %s: %s\n", sourceLabel ? sourceLabel : "?", font_.lastError());
    return false;
  }

  // Scale metrics once; the source stays seekable for per-glyph glyf reads.
  const float scale = static_cast<float>(sizePx_) / static_cast<float>(font_.unitsPerEm());
  int32_t asc = 0, desc = 0, gap = 0;
  font_.fontVMetrics(asc, desc, gap);
  // GfxRenderer positions glyphs as baseline - glyph.top. hhea.ascender is
  // only a typographic metric and can be smaller than the actual CJK outline;
  // use head.yMax as the same actual-top correction used by fontconvert.py.
  const int actualTopPx = static_cast<int>(std::lround(font_.fontBBoxYMax() * scale));
  const int ascPx = std::max(static_cast<int>(std::lround(asc * scale)), actualTopPx);
  const int descPx = static_cast<int>(std::lround(desc * scale));
  int linePx = static_cast<int>(std::lround((asc - desc + gap) * scale));
  if (linePx < 1) linePx = sizePx_;
  if (linePx > 255) linePx = 255;

  data_.bitmap = nullptr;
  data_.glyph = nullptr;
  data_.intervals = nullptr;
  data_.intervalCount = 0;
  data_.advanceY = static_cast<uint8_t>(linePx);
  data_.ascender = ascPx;
  data_.descender = descPx;
  data_.is2Bit = true;

  valid_ = true;
  Serial.printf("[TTF] Loaded %s @%upx (unitsPerEm=%u lineH=%u asc=%d desc=%d slots=%u meta=%u budget=%u)\n",
                sourceLabel ? sourceLabel : "?", sizePx_, font_.unitsPerEm(), data_.advanceY, data_.ascender,
                data_.descender, static_cast<unsigned>(maxSlots_),
                static_cast<unsigned>(sizeof(Entry) * maxSlots_), static_cast<unsigned>(cacheBudget_));
  return true;
}

TtfEpdFont::TtfEpdFont(const String& path, uint16_t sizePx, uint16_t maxSlots, size_t cacheBudget)
    : EpdFont(&data_), path_(path), sizePx_(sizePx), maxSlots_(maxSlots), cacheBudget_(cacheBudget) {
#if defined(ESP32)
  mutex_ = xSemaphoreCreateMutex();
#endif
  if (!allocateEntries()) return;

  auto* s = new SdTtfStream();
  stream_ = s;
  if (!s->open(path_)) {
    Serial.printf("[TTF] Failed to open %s\n", path_.c_str());
    delete s;
    stream_ = nullptr;
    return;
  }
  finishInit(path_.c_str());
}

TtfEpdFont::TtfEpdFont(const uint8_t* data, uint32_t dataSize, uint16_t sizePx, uint16_t maxSlots,
                       size_t cacheBudget)
    : EpdFont(&data_), path_("<flash>"), sizePx_(sizePx), maxSlots_(maxSlots), cacheBudget_(cacheBudget) {
#if defined(ESP32)
  mutex_ = xSemaphoreCreateMutex();
#endif
  if (!data || dataSize == 0) {
    Serial.printf("[TTF] Empty embedded TTF\n");
    return;
  }
  if (!allocateEntries()) return;

  stream_ = new MemoryTtfStream(data, dataSize);
  finishInit("<flash>");
}

TtfEpdFont::~TtfEpdFont() {
  clearCaches();
  if (entries_) {
    for (uint16_t i = 0; i < maxSlots_; ++i) {
      entries_[i].~Entry();
    }
    ttfFree(entries_);
    entries_ = nullptr;
  }
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
  if (!entries_ || slot < 0 || slot >= static_cast<int>(maxSlots_)) return;
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
  if (entries_) {
    for (uint16_t i = 0; i < maxSlots_; ++i) {
      evictSlot(static_cast<int>(i));
    }
  }
  font_.clearScratch();
#if defined(ESP32)
  if (mutex_) xSemaphoreGive(mutex_);
#endif
}

int TtfEpdFont::ensureGlyph(uint32_t cp) const {
  if (!valid_ || !entries_) return -1;

  // Hit.
  for (uint16_t i = 0; i < maxSlots_; ++i) {
    if (entries_[i].cp == cp) {
      entries_[i].lastAccess = ++accessCounter_;
      return static_cast<int>(i);
    }
  }

  uint16_t gid = 0;
  if (!font_.findGlyph(cp, gid)) return -1;
  if (gid == 0 && cp != '?') {
    // Missing codepoint: share the '?' entry (renders '?' like the epdfont
    // backend, and keeps GfxRenderer::hasTextGlyphs' "same as '?'" detection
    // working so UI chrome can fall back to the independent emergency face).
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
  for (uint16_t i = 0; i < maxSlots_; ++i) {
    if (entries_[i].cp == 0xFFFFFFFF) {
      slot = static_cast<int>(i);
      break;
    }
    if (entries_[i].lastAccess < minAccess) {
      minAccess = entries_[i].lastAccess;
      slot = static_cast<int>(i);
    }
  }
  if (slot < 0) return -1;
  evictSlot(slot);

  // Bitmap: allocate from PSRAM (preferred) / heap, bounded by budget.
  uint8_t* bmp = nullptr;
  const uint32_t packedLen = gb.packedLen;
  if (packedLen > 0 && gb.data) {
    bmp = static_cast<uint8_t*>(ttfAlloc(packedLen));
    if (!bmp) {
      Serial.printf("[TTF] glyph alloc %u failed (OOM)\n", packedLen);
      bmp = nullptr;
    } else {
      std::memcpy(bmp, gb.data, packedLen);
    }
  }

  entries_[slot].cp = cp;
  entries_[slot].lastAccess = ++accessCounter_;
  entries_[slot].glyph.width = static_cast<uint8_t>(gb.width);
  entries_[slot].glyph.height = static_cast<uint8_t>(gb.height);
  entries_[slot].glyph.advanceX = static_cast<uint8_t>(std::max(0, std::min(255, static_cast<int>(gb.advance))));
  entries_[slot].glyph.left = gb.xoff;
  entries_[slot].glyph.top = gb.yoff;
  entries_[slot].glyph.dataLength = packedLen;
  entries_[slot].glyph.dataOffset = cp;  // reuses EpdGlyph as the cache key
  entries_[slot].bitmap = bmp;
  entries_[slot].bitmapSize = packedLen;
  cacheBytes_ += packedLen;

  // Bounded budget: evict LRU (sparing the fresh slot) until under budget.
  if (cacheBytes_ > cacheBudget_) {
    for (uint16_t pass = 0; pass < maxSlots_ && cacheBytes_ > cacheBudget_; ++pass) {
      int victim = -1;
      uint32_t la = 0xFFFFFFFF;
      for (uint16_t i = 0; i < maxSlots_; ++i) {
        if (static_cast<int>(i) == slot || entries_[i].cp == 0xFFFFFFFF) continue;
        if (entries_[i].lastAccess < la) {
          la = entries_[i].lastAccess;
          victim = static_cast<int>(i);
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
