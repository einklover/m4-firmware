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

uint32_t runRefresh(bool swapAfter) {
  if (!gDisplay || gRunning) return 0;
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
