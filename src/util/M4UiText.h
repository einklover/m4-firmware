#pragma once

// Unified UI text face for Murphy M4. UI and reader content share the selected
// runtime TTF face: only the reader-size rasterizer/cache is resident and UI
// chrome shrinks those cached glyph bitmaps to its compact layout metrics.
// Legacy epdfont keeps its existing fixed-size lookup behavior.
//
// Pure policy: M4UiTextPolicy.h (host-testable).
// Drawing helpers: this header (device / sim with GfxRenderer).

#include <GfxRenderer.h>
#include <EpdFontFamily.h>
#include <EpdFontLoader.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "fontIds.h"
#include "util/M4UiTextPolicy.h"

namespace M4UiText {

// Legacy generated epdfont chrome sizes. Runtime .ttf deliberately does NOT
// load these faces; it uses the current reader face and GfxRenderer scaling.
inline int uiTtfSizeForLayout(int layoutFontId) {
  return layoutFontId == UI_10_FONT_ID ? 20 : 24;
}

inline bool selectedRuntimeTtf() {
  if (SETTINGS.fontFamily != CrossPointSettings::FONT_CUSTOM || SETTINGS.customFontFamily[0] == '\0') {
    return false;
  }
  std::string name = SETTINGS.customFontFamily;
  if (name.size() < 4) return false;
  std::string ext = name.substr(name.size() - 4);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return ext == ".ttf";
}

// Runtime resolve against GfxRenderer + SETTINGS reader id. For a selected TTF
// this is the central single-face policy: all UI/status text references exactly
// the same font ID/cache as reader body text, then scales it down to the target
// layout face's ascender. No second TTF parser, cmap, stream, mutex or glyph LRU.
inline Face resolve(const GfxRenderer& renderer, int layoutFontId) {
  Face f;
  f.layoutFontId = (layoutFontId == 0) ? UI_12_FONT_ID : layoutFontId;
  f.fontId = f.layoutFontId;
  f.scale = 1.0f;

  if (selectedRuntimeTtf()) {
    const int readerFontId = SETTINGS.getReaderFontId();
    if (readerFontId != -1 && renderer.hasFont(readerFontId)) {
      f.fontId = readerFontId;
      f.scale = renderer.scaleFontToMatch(readerFontId, f.layoutFontId);
    }
  }
  return f;
}

inline Face resolveForText(const GfxRenderer& renderer, int layoutFontId, const char* text,
                           EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  Face f = resolve(renderer, layoutFontId);
  if (SETTINGS.fontFamily != CrossPointSettings::FONT_CUSTOM ||
      strlen(SETTINGS.customFontFamily) == 0) {
    return f;
  }

  const char* safeText = text ? text : "";
  if (selectedRuntimeTtf()) {
    // Reuse the single reader TTF when it covers the label. A runtime TTF may
    // synthesize '?' for a miss, so hasTextGlyphs must decide before rendering.
    if (f.fontId != f.layoutFontId && renderer.hasTextGlyphs(f.fontId, safeText, style)) {
      return f;
    }

    // True fallback must not use UI_10/UI_12/SMALL: in runtime-TTF mode those
    // IDs are scaled views over the same reader face. Prefer the independent
    // NOTOSANS_12 mapping (canonical epdfont when available, builtin subset
    // otherwise), scaled to the requested chrome metrics.
    constexpr int fallbackId = NOTOSANS_12_FONT_ID;
    if (renderer.hasFont(fallbackId) && fallbackId != f.fontId &&
        renderer.hasTextGlyphs(fallbackId, safeText, style)) {
      f.fontId = fallbackId;
      f.scale = renderer.scaleFontToMatch(fallbackId, f.layoutFontId);
      return f;
    }

    // No independent fallback covers the label; keep the selected reader TTF
    // and let its normal '?' behavior surface rather than double-scaling its
    // UI wrapper or allocating another TTF face.
    return f;
  }

  // Legacy epdfont: preserve existing fixed chrome-face behavior.
  const int uiFont = EpdFontLoader::getBestFontId(
      SETTINGS.customFontFamily, uiTtfSizeForLayout(f.layoutFontId));
  if (uiFont != -1 && renderer.hasFont(uiFont) &&
      renderer.hasTextGlyphs(uiFont, safeText, style)) {
    f.fontId = uiFont;
  }
  return f;
}

inline int textWidth(const GfxRenderer& renderer, int layoutFontId, const char* text,
                     EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const Face f = resolveForText(renderer, layoutFontId, text, style);
  return renderer.getTextWidth(f.fontId, text ? text : "", style, f.scale);
}

inline std::string truncated(const GfxRenderer& renderer, int layoutFontId, const char* text, int maxWidth,
                             EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const Face f = resolveForText(renderer, layoutFontId, text, style);
  return renderer.truncatedText(f.fontId, text ? text : "", maxWidth, style, f.scale);
}

inline void draw(const GfxRenderer& renderer, int layoutFontId, int x, int y, const char* text,
                 bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const Face f = resolveForText(renderer, layoutFontId, text, style);
  renderer.drawText(f.fontId, x, y, text ? text : "", black, style, f.scale);
}

// Centered on full screen width (same contract as GfxRenderer::drawCenteredText).
inline void drawCentered(const GfxRenderer& renderer, int layoutFontId, int y, const char* text,
                         bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const Face f = resolveForText(renderer, layoutFontId, text, style);
  const int w = renderer.getTextWidth(f.fontId, text ? text : "", style, f.scale);
  const int x = (renderer.getScreenWidth() - w) / 2;
  renderer.drawText(f.fontId, x, y, text ? text : "", black, style, f.scale);
}

// Draw a chrome label centered inside a touch target.  GfxRenderer::drawText
// takes a top-left y coordinate (not a baseline), so using a hard-coded
// offset causes reader-font labels to sit above/below buttons when a custom
// CJK face has a different ascender.  Resolve the same face used for the
// label, clamp its width, and center both axes from its actual metrics.
inline void drawCenteredInBox(const GfxRenderer& renderer, int layoutFontId, int x, int y, int width, int height,
                              const char* text, bool black = true,
                              EpdFontFamily::Style style = EpdFontFamily::REGULAR, int horizontalPadding = 12) {
  if (width <= 0 || height <= 0) return;
  const Face f = resolveForText(renderer, layoutFontId, text, style);
  const int maxWidth = std::max(1, width - 2 * std::max(0, horizontalPadding));
  const std::string label = renderer.truncatedText(f.fontId, text ? text : "", maxWidth, style, f.scale);
  const int textWidth = renderer.getTextWidth(f.fontId, label.c_str(), style, f.scale);
  const int textHeight = std::max(1, static_cast<int>(std::lround(renderer.getTextHeight(f.fontId) * f.scale)));
  const int textX = x + std::max(0, (width - textWidth) / 2);
  const int textY = y + std::max(0, (height - textHeight) / 2);
  renderer.drawText(f.fontId, textX, textY, label.c_str(), black, style, f.scale);
}

// Align list icons with the actual title/subtitle text block. Native rows are
// taller touch targets, while their text remains top-packed (+4 / +30).
inline int listIconTop(const GfxRenderer& renderer, int layoutFontId, int rowHeight, bool hasSubtitle,
                       int iconSize, int titleTop = 4, int subtitleTop = 30) {
  if (rowHeight <= 0 || iconSize <= 0) return 0;
  // Single-line rows (Files, History, Settings, etc.) use a vertically
  // centered 24px glyph. Their title is intentionally top-packed for the
  // reader-face fallback, so aligning to that logical y would lift the icon
  // above the visible glyph. Only subtitle rows need text-block alignment.
  if (!hasSubtitle) return std::max(0, (rowHeight - iconSize) / 2);
  const Face f = resolve(renderer, layoutFontId);
  const int lineHeight = std::max(1, static_cast<int>(std::lround(renderer.getTextHeight(f.fontId) * f.scale)));
  const int blockTop = titleTop;
  const int blockBottom = hasSubtitle ? subtitleTop + lineHeight : titleTop + lineHeight;
  const int blockCenter = blockTop + std::max(0, blockBottom - blockTop) / 2;
  return std::max(0, blockCenter - iconSize / 2);
}

// Chapter-list style row: use the same runtime reader face scaled to chrome
// metrics (or the layout face when no runtime TTF is selected).
inline Face resolveChapterRow(const GfxRenderer& renderer, int chromeFontId) {
  return resolve(renderer, chromeFontId);
}

}  // namespace M4UiText
