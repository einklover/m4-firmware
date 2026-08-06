#include "M4WaveformLab.h"

#include <HalDisplay.h>
#include <SDCardManager.h>

#include <cstring>

#if defined(ARDUINO_ARCH_ESP32)
#include "esp_heap_caps.h"
#endif

namespace M4WaveformLab {

namespace {

HalDisplay* gDisplay = nullptr;

// Two PSRAM frame slots (old / new).
uint8_t* gSlot[2] = {nullptr, nullptr};
size_t gSlotReceived[2] = {0, 0};
bool gUploadTarget = false;
int gUploadSlot = 0;

// SD-backed frames (kindle-style page-turn sequences): when set, runRefresh
// reads prev/next from SD instead of the uploaded PSRAM slots.
char gSdPrev[96] = {};
char gSdNext[96] = {};
bool gSdFramesSet = false;

// 110-byte LUT (105 waveform + 5 voltage tail).  Voltage tail locked by
// default: experiments must not move VGH/VSH/VSL/VCOM until explicitly
// unlocked (LUT editor in the host tool can still send them, but the device
// refuses by default).
uint8_t gLut[kLutBytes] = {};
bool gLutSet = false;
bool gVoltagesUnlocked = false;
bool gRunning = false;

uint32_t gLastRunMs = 0;
uint32_t gRuns = 0;

uint8_t* allocSlot() {
#if defined(ARDUINO_ARCH_ESP32)
  auto* p = static_cast<uint8_t*>(heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (p) return p;
#endif
  return static_cast<uint8_t*>(malloc(kFrameBytes));
}

}  // namespace

void setDisplay(HalDisplay* display) { gDisplay = display; }

bool setLutBytes(const uint8_t* lut, size_t len) {
  if (!lut || len < 105) return false;
  return setLut(lut, len, /*unlockVoltages=*/false);
}

bool beginFrameUpload(int slot) {
  if (gRunning) return false;
  if (slot < 0 || slot > 1) return false;
  if (!gSlot[slot]) gSlot[slot] = allocSlot();
  if (!gSlot[slot]) return false;
  gSlotReceived[slot] = 0;
  gUploadTarget = true;
  gUploadSlot = slot;
  return true;
}

uint8_t* frameSlot(int slot) {
  if (slot < 0 || slot > 1) return nullptr;
  return gSlot[slot];
}

bool frameUploadComplete(int slot) {
  if (slot < 0 || slot > 1) return false;
  return gSlot[slot] && gSlotReceived[slot] == kFrameBytes;
}

// Called by the bridge chunk path: appends `data` to the active upload slot.
bool frameChunkAppend(const uint8_t* data, size_t len) {
  if (!gUploadTarget) return false;
  const int slot = gUploadSlot;
  if (!gSlot[slot]) return false;
  if (gSlotReceived[slot] + len > kFrameBytes) return false;
  std::memcpy(gSlot[slot] + gSlotReceived[slot], data, len);
  gSlotReceived[slot] += len;
  if (gSlotReceived[slot] == kFrameBytes) {
    gUploadTarget = false;  // slot complete
  }
  return true;
}

// Bridge calls this when the declared chunk total is reached.
void endFrameUpload(int slot) {
  (void)slot;
  gUploadTarget = false;
}

void swapSlots() {
  if (gSlot[0] && gSlot[1]) {
    std::swap(gSlot[0], gSlot[1]);
    gSlotReceived[0] = gSlotReceived[1] = kFrameBytes;
  }
}

bool setSdFrames(const char* prevPath, const char* nextPath) {
  if (!prevPath || !nextPath) return false;
  if (std::strlen(prevPath) >= sizeof(gSdPrev) || std::strlen(nextPath) >= sizeof(gSdNext)) return false;
  // Verify both files exist and are exactly one frame before accepting.
  FsFile f;
  if (!SDCardManager::getInstance().openFileForRead("LAB", prevPath, f)) return false;
  const size_t prevSize = static_cast<size_t>(f.size());
  f.close();
  if (prevSize != kFrameBytes) return false;
  if (!SDCardManager::getInstance().openFileForRead("LAB", nextPath, f)) return false;
  const size_t nextSize = static_cast<size_t>(f.size());
  f.close();
  if (nextSize != kFrameBytes) return false;
  std::strncpy(gSdPrev, prevPath, sizeof(gSdPrev) - 1);
  std::strncpy(gSdNext, nextPath, sizeof(gSdNext) - 1);
  gSdFramesSet = true;
  return true;
}

namespace {

// Read a full frame from SD into the PSRAM slot (allocate if needed).
bool readFrameIntoSlot(const char* path, int slot) {
  if (!gSlot[slot]) gSlot[slot] = allocSlot();
  if (!gSlot[slot]) return false;
  FsFile f;
  if (!SDCardManager::getInstance().openFileForRead("LAB", path, f)) return false;
  const size_t n = static_cast<size_t>(f.size());
  if (n != kFrameBytes) {
    f.close();
    return false;
  }
  if (f.read(gSlot[slot], kFrameBytes) != static_cast<int>(kFrameBytes)) {
    f.close();
    return false;
  }
  f.close();
  gSlotReceived[slot] = kFrameBytes;
  return true;
}

}  // namespace

bool baselineFromSd(const char* framePath) {
  if (!gDisplay || gRunning) return false;
  if (!readFrameIntoSlot(framePath, 0)) return false;
  gRunning = true;
  // Absolute FULL refresh: rewrites both planes from the frame, independent
  // of what the panel currently shows, so the differential baseline is exact.
  gDisplay->waveformLabBaseline(gSlot[0]);
  gRunning = false;
  return true;
}

bool setLut(const uint8_t* lut, size_t len, bool unlockVoltages) {
  if (!lut || len < 105) return false;
  std::memcpy(gLut, lut, kLutBytes);
  if (!unlockVoltages) {
    // Voltage bytes (105..109) locked: force the board's known-safe factory
    // tail (VGH/VSH1/VSH2/VSL/VCOM) from the stock grayscale LUT instead of
    // letting an experiment move the panel rails.  Unlock only when tuning
    // voltages deliberately.
    static const uint8_t kSafeVoltageTail[kLutBytes - 105] = {0x17, 0x41, 0xA8, 0x32, 0x30};
    std::memcpy(gLut + 105, kSafeVoltageTail, sizeof(kSafeVoltageTail));
  }
  gVoltagesUnlocked = unlockVoltages;
  gLutSet = true;
  return true;
}

namespace {

// Compose one wipe frame: right `edge` columns are new page, left is old;
// `feather` px around the edge use a sparse dithered transition so the slide
// reads smooth instead of a dense checkered fence.  edge>=W: full new page;
// edge<=0: full old page.
// dir: 0=right->left (new page enters from right), 1=left->right,
//      2=bottom->top (new page enters from bottom), 3=top->bottom.
void composeWipeDir(const uint8_t* old, const uint8_t* newFrame, uint8_t* out, int edge, int feather,
                    int widthBytes, int height, int dir) {
  const int W = widthBytes * 8;
  const int H = height;
  const bool horiz = (dir == 0 || dir == 1);
  const int span = horiz ? W : H;
  // When the edge has fully swept, the whole frame is the new page.
  if (edge <= 0) {
    std::memcpy(out, newFrame, static_cast<size_t>(widthBytes) * height);
    return;
  }
  if (edge >= span) {
    std::memcpy(out, old, static_cast<size_t>(widthBytes) * height);
    return;
  }
  for (int row = 0; row < H; ++row) {
    const int rowOff = row * widthBytes;
    for (int x = 0; x < W; ++x) {
      int v = 0;
      const int bx = x / 8;
      const int bit = 0x80 >> (x % 8);
      int pos;
      if (horiz) {
        pos = (dir == 0) ? x : (W - 1 - x);
      } else {
        pos = (dir == 2) ? row : (H - 1 - row);
      }
      const uint8_t* src = (pos < edge - feather) ? old : ((pos >= edge) ? newFrame : nullptr);
      if (src != nullptr) {
        v = (src[rowOff + bx] & bit) ? 1 : 0;
      } else {
        // Sparse blend inside the feather band.
        const int d = pos - (edge - feather);
        const uint8_t* s = ((d & 1) == 0) ? newFrame : old;
        v = (s[rowOff + bx] & bit) ? 1 : 0;
      }
      if (v) out[rowOff + bx] |= bit;
    }
  }
}

void composeWipe(const uint8_t* old, const uint8_t* newFrame, uint8_t* out, int edge, int feather,
                 int widthBytes, int height) {
  composeWipeDir(old, newFrame, out, edge, feather, widthBytes, height, /*dir=*/0);
}

}  // namespace

uint32_t runAnimateMem(const uint8_t* oldFrame, const uint8_t* newFrame, int steps, int feather,
                       uint32_t tailMs, int dir) {
  if (!gDisplay || gRunning) return 0;
  if (!gLutSet) return 0;
  if (!oldFrame || !newFrame) return 0;
  if (steps < 1) steps = 1;
  if (steps > 64) steps = 64;
  if (feather < 0) feather = 0;
  if (feather > 64) feather = 64;
  uint8_t* cur = allocSlot();
  if (!cur) return 0;
  uint8_t* spare = allocSlot();
  if (!spare) {
    free(cur);
    return 0;
  }
  const int W = 800;
  const int wb = W / 8;
  const int H = 480;
  const uint32_t t0 = millis();
  gRunning = true;
  // Same multipass wipe as runAnimate: RED stays the ORIGINAL old frame,
  // BW is the composed wipe (old left / new right), covered region re-drives
  // every step so ink keeps settling.  oldFrame/newFrame must stay valid for
  // the duration (they are the renderer's frame buffer).
  for (int i = 1; i <= steps; ++i) {
    std::memset(cur, 0, kFrameBytes);
    const int span = (dir == 0 || dir == 1) ? W : H;
    const int edge = span - (span * i) / steps;
    composeWipeDir(oldFrame, newFrame, cur, edge, feather, wb, H, dir);
    gDisplay->waveformLabRefresh(oldFrame, cur, gLut, /*turnOff=*/false);
    delay(2);
  }
  if (tailMs > 0) {
    uint32_t target = tailMs;
    if (target > 10000) target = 10000;
    const uint32_t tailStart = millis();
    int tail = 0;
    while (tail < 12 && (millis() - tailStart) < target) {
      gDisplay->waveformLabRefresh(oldFrame, newFrame, gLut, /*turnOff=*/false);
      ++tail;
      delay(2);
    }
  }
  free(cur);
  free(spare);
  gRunning = false;
  return millis() - t0;
}

uint32_t runAnimate(const char* prevPath, const char* nextPath, int steps, int feather, uint32_t tailMs,
                    int dir) {
  if (!gDisplay || gRunning) return 0;
  if (!gLutSet) return 0;
  if (steps < 1) steps = 1;
  if (steps > 64) steps = 64;
  if (feather < 0) feather = 0;
  if (feather > 64) feather = 64;
  if (!readFrameIntoSlot(prevPath, 0)) return 0;
  if (!readFrameIntoSlot(nextPath, 1)) return 0;
  // BW plane buffer for the composed wipe frame (RED stays original page1).
  uint8_t* synth = allocSlot();
  if (!synth) return 0;

  const int W = 800;
  const int wb = W / 8;
  const int H = 480;
  const uint32_t t0 = millis();
  gRunning = true;
  // Kindle-style multipass wipe:
  //   RED  = original page1 every step (never the previous synth)
  //   BW   = composeWipe(page1, page2, edge)
  // Covered region (x >= edge): RED=page1 ≠ BW=page2 → drives EVERY step,
  // so already-wiped ink keeps settling ("变重") instead of one-shot freeze.
  // Uncovered region (x < edge): RED=BW=page1 → idle.
  // Intermediate frames always come from the ORIGINAL prev/next — never chain
  // synth→synth (that accumulates blend error / ghosting).
  for (int i = 1; i <= steps; ++i) {
    std::memset(synth, 0, kFrameBytes);
    // New page enters from the sweep direction: edge sweeps span->0.
    const int span = (dir == 0 || dir == 1) ? W : H;
    const int edge = span - (span * i) / steps;
    composeWipeDir(gSlot[0], gSlot[1], synth, edge, feather, wb, H, dir);
    gDisplay->waveformLabRefresh(gSlot[0], synth, gLut, /*turnOff=*/false);
    delay(2);  // yield WDT / other tasks
  }
  // Ghost-clearing tail: AFTER the wipe animation, keep driving the full
  // old->new differential for `tailMs` wall time (measured from the end of
  // the animation, not the start), so residual old-page ink keeps settling.
  // RED stays the ORIGINAL page1, BW is the FINAL page2 — every changed
  // pixel is re-driven each pass (multipass settling).
  if (tailMs > 0) {
    uint32_t target = tailMs;
    if (target > 10000) target = 10000;
    const uint32_t tailStart = millis();
    int tail = 0;
    while (tail < 12 && (millis() - tailStart) < target) {
      gDisplay->waveformLabRefresh(gSlot[0], gSlot[1], gLut, /*turnOff=*/false);
      ++tail;
      delay(2);
    }
#if defined(ARDUINO_ARCH_ESP32)
    Serial.printf("[LAB] ghost-clear tail passes=%d\n", tail);
#endif
  }
  free(synth);
  gRunning = false;
  const uint32_t total = millis() - t0;
  ++gRuns;
  gLastRunMs = total;
  {
    FsFile f;
    if (SDCardManager::getInstance().openFileForWrite("LAB", "/waveform/lab_diag.txt", f)) {
      char line[128];
      snprintf(line, sizeof(line),
               "animate kindle multipass steps=%d feather=%d total=%ums\n", steps, feather,
               static_cast<unsigned>(total));
      f.write(line, strlen(line));
      f.close();
    }
  }
  return total;
}

namespace {

// Async animation session (pumped from the main loop so the loop never blocks
// for the whole animation — blocking starves the WDT / e-ink tasks).
// mode: 0 = full-frame Kindle multipass (lut_animate)
//       1 = window multipass of the growing covered region (lut_wipe)
struct AnimSession {
  bool active = false;
  int mode = 0;
  int steps = 0;
  int cur = 0;
  int feather = 0;
  int W = 800;
  int wb = 100;
  int H = 480;
  uint8_t* synth = nullptr;  // mode 0 only: BW wipe buffer
  uint32_t t0 = 0;
};
AnimSession gAnim;

}  // namespace

bool startAnimateWindow(const char* prevPath, const char* nextPath, int steps) {
  return startAnimate(prevPath, nextPath, steps, 0, /*windowMode=*/true);
}

bool startAnimate(const char* prevPath, const char* nextPath, int steps, int feather, bool windowMode) {
  if (gRunning || gAnim.active) return false;
  if (!gLutSet) return false;
  if (steps < 1) steps = 1;
  if (steps > 64) steps = 64;
  if (feather < 0) feather = 0;
  if (feather > 64) feather = 64;
  if (!readFrameIntoSlot(prevPath, 0)) return false;
  if (!readFrameIntoSlot(nextPath, 1)) return false;
  if (!windowMode) {
    gAnim.synth = allocSlot();
    if (!gAnim.synth) return false;
  }
  gAnim.active = true;
  gAnim.mode = windowMode ? 1 : 0;
  gAnim.steps = steps;
  gAnim.cur = 1;
  gAnim.feather = feather;
  gAnim.t0 = millis();
  gRunning = true;
  return true;
}

bool pumpAnimateWindow(uint32_t& stepMsOut) {
  if (!gAnim.active) return false;
  if (gAnim.cur > gAnim.steps) {
    gAnim.active = false;
    gRunning = false;
    if (gAnim.synth) {
      free(gAnim.synth);
      gAnim.synth = nullptr;
    }
    ++gRuns;
    gLastRunMs = millis() - gAnim.t0;
    return false;
  }
  if (gAnim.mode == 0) {
    // Full-frame Kindle multipass: RED always original page1, BW = wipe.
    // Never swap synth into gSlot[0] — originals must stay intact.
    if (!gAnim.synth) return false;
    std::memset(gAnim.synth, 0, kFrameBytes);
    const int edge = gAnim.W - (gAnim.W * gAnim.cur) / gAnim.steps;
    composeWipe(gSlot[0], gSlot[1], gAnim.synth, edge, gAnim.feather, gAnim.wb, gAnim.H);
    gAnim.cur++;
    const uint32_t t0 = millis();
    gDisplay->waveformLabRefresh(gSlot[0], gAnim.synth, gLut, /*turnOff=*/false);
    stepMsOut = millis() - t0;
    return true;
  }
  // Window multipass: refresh the ENTIRE covered region [edge, W] each step
  // with RED=page1 / BW=page2 (real pages).  Already-covered strips are
  // rewritten and re-driven every later step → progressive ink settling.
  // (Old path only refreshed the newly entered strip once — one-shot freeze.)
  int edge = gAnim.W - (gAnim.W * gAnim.cur) / gAnim.steps;
  edge &= ~7;  // byte-align for SSD1677 window
  if (gAnim.cur == gAnim.steps) edge = 0;
  gAnim.cur++;
  const int w = gAnim.W - edge;
  if (w <= 0) {
    stepMsOut = 0;
    return true;
  }
  const uint32_t t0 = millis();
  gDisplay->waveformLabRefreshWindow(gSlot[0], gSlot[1], gLut,
                                     static_cast<uint16_t>(edge), 0,
                                     static_cast<uint16_t>(w), static_cast<uint16_t>(gAnim.H));
  stepMsOut = millis() - t0;
  return true;
}

bool animateActive() { return gAnim.active; }

uint32_t runAnimateWindow(const char* prevPath, const char* nextPath, int steps) {
  // Keep the blocking path for CLI/scripts; the GUI uses the async session.
  if (!startAnimateWindow(prevPath, nextPath, steps)) return 0;
  uint32_t stepMs = 0;
  while (pumpAnimateWindow(stepMs)) {
    // Feed the watchdog and yield so the system stays alive during a long
    // animation driven from the main loop.
    delay(2);
  }
  return gLastRunMs;
}

uint32_t runSettle(const char* prevPath, const char* nextPath) {
  if (!gDisplay || gRunning) return 0;
  if (!gLutSet) return 0;
  if (!readFrameIntoSlot(prevPath, 0)) return 0;
  if (!readFrameIntoSlot(nextPath, 1)) return 0;
  gRunning = true;
  const uint32_t t0 = millis();
  // Full-frame differential: RED=old, BW=new -> drive every changed pixel
  // one more time with the (stronger) currently loaded SETTLE LUT.
  gDisplay->waveformLabRefresh(gSlot[0], gSlot[1], gLut, /*turnOff=*/false);
  gRunning = false;
  const uint32_t total = millis() - t0;
  ++gRuns;
  gLastRunMs = total;
  return total;
}

uint32_t runRefresh(bool swapAfter) {  if (!gDisplay || gRunning) return 0;
  if (!gLutSet) return 0;
  if (gSdFramesSet) {
    if (!readFrameIntoSlot(gSdPrev, 0) || !readFrameIntoSlot(gSdNext, 1)) return 0;
  } else {
    if (!gSlot[0] || !gSlot[1]) return 0;
    if (!frameUploadComplete(0) || !frameUploadComplete(1)) return 0;
  }
  gRunning = true;
  // prev = slot0 (old), next = slot1 (new).
  gLastRunMs = gDisplay->waveformLabRefresh(gSlot[0], gSlot[1], gLut, /*turnOff=*/false);
  if (swapAfter) {
    std::swap(gSlot[0], gSlot[1]);
    gSlotReceived[0] = gSlotReceived[1] = kFrameBytes;
  }
  gRunning = false;
  ++gRuns;
  // Diagnostics: append one line to /waveform/lab_diag.txt (SD), so the host
  // can read the actual refresh path taken without racing the log stream.
  {
    FsFile f;
    if (SDCardManager::getInstance().openFileForWrite("LAB", "/waveform/lab_diag.txt", f)) {
      char line[96];
      snprintf(line, sizeof(line), "run=%u ms=%u lut_set=%d\n", static_cast<unsigned>(gRuns),
               static_cast<unsigned>(gLastRunMs), gLutSet ? 1 : 0);
      f.write(line, strlen(line));
      f.close();
    }
  }
  return gLastRunMs;
}

void clearAll() {
  if (!gDisplay) return;
  if (gRunning) return;
  // Safe recovery: full refresh of the current framebuffer content via the
  // standard HAL path, then drop all experiment state.
  gRunning = true;
  gDisplay->refreshDisplay(HalDisplay::FULL_REFRESH);
  gRunning = false;
  gLutSet = false;
  gUploadTarget = false;
  gSdFramesSet = false;
  gRuns = 0;
  gLastRunMs = 0;
}

Stats stats() {
  Stats s;
  s.lastRunMs = gLastRunMs;
  s.runs = gRuns;
  s.lutSet = gLutSet;
  s.active = gLutSet;
  s.framesReady = frameUploadComplete(0) && frameUploadComplete(1);
  return s;
}

}  // namespace M4WaveformLab
