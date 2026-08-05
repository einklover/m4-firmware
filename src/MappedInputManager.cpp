#include "MappedInputManager.h"

#include <Arduino.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "util/TouchHitGeometry.h"

namespace {
using ButtonIndex = uint8_t;

struct SideLayoutMap {
  ButtonIndex pageBack;
  ButtonIndex pageForward;
};

// Order matches CrossPointSettings::SIDE_BUTTON_LAYOUT.
constexpr SideLayoutMap kSideLayouts[] = {
    {HalGPIO::BTN_UP, HalGPIO::BTN_DOWN},
    {HalGPIO::BTN_DOWN, HalGPIO::BTN_UP},
};

constexpr float TOP_EDGE_MENU_GESTURE_FRAC_Y = 0.14f;
constexpr unsigned long TOUCH_DOWN_SELECT_DELAY_MS = 90;
}  // namespace

MappedInputManager::MappedInputManager(HalGPIO& gpioRef) : gpio(gpioRef), renderer(nullptr) {}

void MappedInputManager::beginFrame() {
  swipeCacheValid = false;
  swipeCacheHas = false;
  syntheticBack = false;
  touchHeldOverrideValid = false;
#if defined(CROSSPOINT_MURPHY_M4)
  // Consume one-frame synthetic events exactly once per beginFrame().
  synthKind_ = SynthKind::None;
#endif
}

void MappedInputManager::pulseSyntheticBack() { syntheticBack = true; }

#if defined(CROSSPOINT_MURPHY_M4)
bool MappedInputManager::hasPendingSyntheticInput() const { return synthKind_ != SynthKind::None; }

bool MappedInputManager::injectSyntheticTap(int x, int y, bool& busy) {
  busy = false;
  if (!renderer) {
    return false;
  }
  const int w = renderer->getScreenWidth();
  const int h = renderer->getScreenHeight();
  if (x < 0 || y < 0 || x >= w || y >= h) {
    return false;
  }
  const unsigned long now = millis();
  // Do not treat synthLastInjectMs_==0 as a recent inject (first command after boot).
  const bool rateBusy = synthEverInjected_ && (now - synthLastInjectMs_) < kSynthMinIntervalMs;
  if (synthKind_ != SynthKind::None || rateBusy) {
    busy = true;
    return false;
  }
  synthKind_ = SynthKind::Tap;
  synthTapX_ = x;
  synthTapY_ = y;
  synthLastInjectMs_ = now;
  synthEverInjected_ = true;
  return true;
}

bool MappedInputManager::injectSyntheticKey(Button button, bool& busy) {
  busy = false;
  const unsigned long now = millis();
  const bool rateBusy = synthEverInjected_ && (now - synthLastInjectMs_) < kSynthMinIntervalMs;
  if (synthKind_ != SynthKind::None || rateBusy) {
    busy = true;
    return false;
  }
  synthKind_ = SynthKind::Key;
  synthKey_ = button;
  synthLastInjectMs_ = now;
  synthEverInjected_ = true;
  return true;
}
#endif

bool MappedInputManager::mapButton(const Button button, bool (HalGPIO::*fn)(uint8_t) const) const {
  const auto sideLayout = static_cast<CrossPointSettings::SIDE_BUTTON_LAYOUT>(SETTINGS.sideButtonLayout);
  const auto& side = kSideLayouts[sideLayout];

  switch (button) {
    case Button::Back:
      // Logical Back maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonBack);
    case Button::Confirm:
      // Logical Confirm maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonConfirm);
    case Button::Left:
      // Logical Left maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonLeft);
    case Button::Right:
      // Logical Right maps to user-configured front button.
      return (gpio.*fn)(SETTINGS.frontButtonRight);
    case Button::Up:
      // Side buttons remain fixed for Up/Down.
      return (gpio.*fn)(HalGPIO::BTN_UP);
    case Button::Down:
      // Side buttons remain fixed for Up/Down.
      return (gpio.*fn)(HalGPIO::BTN_DOWN);
    case Button::Power:
      // Power button bypasses remapping.
      return (gpio.*fn)(HalGPIO::BTN_POWER);
    case Button::PageBack:
      // Reader page navigation uses side buttons and can be swapped via settings.
      return (gpio.*fn)(side.pageBack);
    case Button::PageForward:
      // Reader page navigation uses side buttons and can be swapped via settings.
      return (gpio.*fn)(side.pageForward);
  }

  return false;
}

bool MappedInputManager::wasPressed(const Button button) const {
  if (button == Button::Back && syntheticBack) return true;
#if defined(CROSSPOINT_MURPHY_M4)
  if (synthKind_ == SynthKind::Key && synthKey_ == button) return true;
#endif
  return mapButton(button, &HalGPIO::wasPressed);
}

bool MappedInputManager::wasReleased(const Button button) const {
  // Pulse both pressed+released so activities using either edge work in one frame.
  if (button == Button::Back && syntheticBack) return true;
#if defined(CROSSPOINT_MURPHY_M4)
  if (synthKind_ == SynthKind::Key && synthKey_ == button) return true;
#endif
  return mapButton(button, &HalGPIO::wasReleased);
}

bool MappedInputManager::isPressed(const Button button) const { return mapButton(button, &HalGPIO::isPressed); }

bool MappedInputManager::wasAnyPressed() const {
  return gpio.wasAnyPressed() || gpio.wasTouchActivity();
}

bool MappedInputManager::wasAnyReleased() const { return gpio.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const { return gpio.getHeldTime(); }

MappedInputManager::Labels MappedInputManager::mapLabels(const char* back, const char* confirm, const char* previous,
                                                         const char* next) const {
  // Build the label order based on the configured hardware mapping.
  auto labelForHardware = [&](uint8_t hw) -> const char* {
    // Compare against configured logical roles and return the matching label.
    if (hw == SETTINGS.frontButtonBack) {
      return back;
    }
    if (hw == SETTINGS.frontButtonConfirm) {
      return confirm;
    }
    if (hw == SETTINGS.frontButtonLeft) {
      return previous;
    }
    if (hw == SETTINGS.frontButtonRight) {
      return next;
    }
    return "";
  };

  return {labelForHardware(HalGPIO::BTN_BACK), labelForHardware(HalGPIO::BTN_CONFIRM),
          labelForHardware(HalGPIO::BTN_LEFT), labelForHardware(HalGPIO::BTN_RIGHT)};
}

int MappedInputManager::getPressedFrontButton() const {
  // Scan the raw front buttons in hardware order.
  // This bypasses remapping so the remap activity can capture physical presses.
  if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
    return HalGPIO::BTN_BACK;
  }
  if (gpio.wasPressed(HalGPIO::BTN_CONFIRM)) {
    return HalGPIO::BTN_CONFIRM;
  }
  if (gpio.wasPressed(HalGPIO::BTN_LEFT)) {
    return HalGPIO::BTN_LEFT;
  }
  if (gpio.wasPressed(HalGPIO::BTN_RIGHT)) {
    return HalGPIO::BTN_RIGHT;
  }
  return -1;
}

bool MappedInputManager::hasTouch() const { return gpio.hasTouch() && renderer != nullptr; }

bool MappedInputManager::wasScreenTapped(int& x, int& y) const {
#if defined(CROSSPOINT_MURPHY_M4)
  if (synthKind_ == SynthKind::Tap) {
    x = synthTapX_;
    y = synthTapY_;
    return true;
  }
#endif
  if (!hasTouch()) return false;
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.wasTouchTap(nx, ny)) return false;
  renderer->tapToLogical(nx, ny, x, y);
  rememberTouchHeldTime();
  return true;
}

bool MappedInputManager::wasScreenTouchDown(int& x, int& y) const {
  if (!hasTouch()) return false;
  float nx = 0.0f;
  float ny = 0.0f;
  unsigned long heldMs = 0;
  if (!gpio.isTouchTapCandidate(nx, ny, heldMs)) return false;
  if (heldMs < TOUCH_DOWN_SELECT_DELAY_MS) return false;
  renderer->tapToLogical(nx, ny, x, y);
  return true;
}

bool MappedInputManager::isScreenTouchHeld(int& x, int& y) const {
  if (!hasTouch()) return false;
  float nx = 0.0f;
  float ny = 0.0f;
  if (!gpio.isTouchHeldAt(nx, ny)) return false;
  renderer->tapToLogical(nx, ny, x, y);
  return true;
}

unsigned long MappedInputManager::lastScreenTouchHeldMs() const {
  if (!hasTouch()) return 0;
  if (touchHeldOverrideValid && millis() - touchHeldOverrideAt <= 250) return touchHeldOverrideMs;
  touchHeldOverrideValid = false;
  return gpio.lastTouchHeldMs();
}

void MappedInputManager::rememberTouchHeldTime() const {
  touchHeldOverrideValid = true;
  touchHeldOverrideMs = gpio.lastTouchHeldMs();
  touchHeldOverrideAt = millis();
}

bool MappedInputManager::wasTapInRect(const int x, const int y, const int width, const int height) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) && tx >= x && tx < x + width && ty >= y && ty < y + height;
}

bool MappedInputManager::listItemFromPoint(const int x, const int y, int& index, const int itemCount,
                                           const int selectedIndex, const int listTop, const int listHeight,
                                           const bool hasSubtitle) const {
  (void)x;
  if (itemCount <= 0 || !renderer) return false;
  if (y < listTop || y >= listTop + listHeight) return false;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int rowStep = hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  if (rowStep <= 0) return false;

  const int pageItems = std::max(1, listHeight / rowStep);
  const int pageStart = std::max(0, selectedIndex / pageItems) * pageItems;
  const int row = (y - listTop) / rowStep;
  const int tapped = pageStart + row;
  if (row < 0 || row >= pageItems || tapped >= itemCount) return false;
  index = tapped;
  return true;
}

bool MappedInputManager::wasListItemTapped(int& index, const int itemCount, const int selectedIndex, const int listTop,
                                           const int listHeight, const bool hasSubtitle) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTapped(tx, ty) &&
         listItemFromPoint(tx, ty, index, itemCount, selectedIndex, listTop, listHeight, hasSubtitle);
}

bool MappedInputManager::wasListItemTouchedDown(int& index, const int itemCount, const int selectedIndex,
                                                const int listTop, const int listHeight, const bool hasSubtitle) const {
  int tx = 0;
  int ty = 0;
  return wasScreenTouchDown(tx, ty) &&
         listItemFromPoint(tx, ty, index, itemCount, selectedIndex, listTop, listHeight, hasSubtitle);
}

MappedInputManager::RowTouch MappedInputManager::rowTouch(int& row, const int top, const int rowStep,
                                                          const int rowCount, const int xStart, const int xEnd,
                                                          const int rowHeight) const {
  if (rowStep <= 0 || rowCount <= 0) return RowTouch::None;
  const auto hit = [&](const int x, const int y) {
    if (x < xStart || x >= xEnd || y < top) return false;
    const int r = (y - top) / rowStep;
    if (r >= rowCount) return false;
    if (rowHeight > 0 && (y - top) % rowStep >= rowHeight) return false;
    row = r;
    return true;
  };
  int x = 0;
  int y = 0;
  if (wasScreenTouchDown(x, y) && hit(x, y)) return RowTouch::Down;
  if (wasScreenTapped(x, y) && hit(x, y)) return RowTouch::Tap;
  return RowTouch::None;
}

bool MappedInputManager::decodeSwipe(int& sx, int& sy, int& ex, int& ey) const {
  if (!hasTouch()) return false;
  if (!swipeCacheValid) {
    swipeCacheValid = true;
    float nxs = 0.0f, nys = 0.0f, nxe = 0.0f, nye = 0.0f;
    swipeCacheHas = gpio.wasSwipe(nxs, nys, nxe, nye);
    if (swipeCacheHas) {
      renderer->tapToLogical(nxs, nys, swipeSx, swipeSy);
      renderer->tapToLogical(nxe, nye, swipeEx, swipeEy);
    }
  }
  if (!swipeCacheHas) return false;
  sx = swipeSx;
  sy = swipeSy;
  ex = swipeEx;
  ey = swipeEy;
  return true;
}

MappedInputManager::SwipeDir MappedInputManager::wasSwipe() const {
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return SwipeDir::None;
  // System nav gestures are not generic list/page swipes
  const int w = renderer->getScreenWidth();
  const int h = renderer->getScreenHeight();
  if (TouchHitGeometry::isSystemBackSwipe(sx, sy, ex, ey, w, h)) return SwipeDir::None;
  if (TouchHitGeometry::isSystemHomeSwipe(sx, sy, ex, ey, w, h)) return SwipeDir::None;
  const int dx = ex - sx;
  const int dy = ey - sy;
  if (std::abs(dx) >= std::abs(dy)) {
    return dx < 0 ? SwipeDir::Left : SwipeDir::Right;
  }
  return dy < 0 ? SwipeDir::Up : SwipeDir::Down;
}

bool MappedInputManager::wasBackGesture() const {
  if (!hasTouch()) return false;
  int sx = 0, sy = 0, ex = 0, ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const bool hit = TouchHitGeometry::isSystemBackSwipe(sx, sy, ex, ey, renderer->getScreenWidth(),
                                                       renderer->getScreenHeight());
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasHomeGesture() const {
  if (!hasTouch()) return false;
  int sx = 0, sy = 0, ex = 0, ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  const bool hit = TouchHitGeometry::isSystemHomeSwipe(sx, sy, ex, ey, renderer->getScreenWidth(),
                                                       renderer->getScreenHeight());
  if (hit) rememberTouchHeldTime();
  return hit;
}

bool MappedInputManager::wasMenuGesture() const {
  if (!hasTouch()) return false;
  int sx = 0;
  int sy = 0;
  int ex = 0;
  int ey = 0;
  if (!decodeSwipe(sx, sy, ex, ey)) return false;
  // Don't steal system home/back
  const int w = renderer->getScreenWidth();
  const int h = renderer->getScreenHeight();
  if (TouchHitGeometry::isSystemBackSwipe(sx, sy, ex, ey, w, h)) return false;
  if (TouchHitGeometry::isSystemHomeSwipe(sx, sy, ex, ey, w, h)) return false;
  return sy <= renderer->getScreenHeight() * TOP_EDGE_MENU_GESTURE_FRAC_Y && ey > sy &&
         std::abs(ey - sy) > std::abs(ex - sx);
}

bool MappedInputManager::wasTouchActivity() const { return gpio.wasTouchActivity(); }
