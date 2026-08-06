#pragma once

// M4 Waveform Lab: USB-driven SSD1677 LUT experimentation.
// Hidden feature — only reachable over the debug bridge (no UI entry).
//
// Commands (all plain JSON over the M4SerialDebug bridge):
//   lut_begin  {slot:0|1, size:48000}            start frame upload (chunks follow)
//   lut_upload {lut:"<b64 110 bytes>"}            set the experiment LUT
//   lut_set_frames {prev:"path", next:"path"}     SD frame paths (run reads from SD)
//   lut_run    {mode:"fast"}                      refresh prev->next with current LUT
//   lut_swap                                     swap slots (next becomes prev)
//   lut_stats                                    {last_ms, lut_set, frames}
//   lut_clear  {mode:"full"}                      safe recovery refresh
//   lut_end                                      leave experiment mode
//
// Chunks uploaded after lut_begin land in the PSRAM frame slot; the bridge's
// chunk session writes here instead of SD. Safety: voltage bytes (105..109)
// are force-kept at the board's factory values unless explicitly unlocked;
// a full-refresh recovery is always available.

#include <cstddef>
#include <cstdint>

class HalDisplay;

namespace M4WaveformLab {

inline constexpr size_t kFrameBytes = 48000;  // 800x480 1bpp
inline constexpr size_t kLutBytes = 110;

struct Stats {
  uint32_t lastRunMs = 0;
  uint32_t runs = 0;
  bool lutSet = false;
  bool active = false;
  bool framesReady = false;
};

// Call from the debug bridge.  Returns a JSON error key or nullptr on success.
// `display` must be the live HalDisplay (NULL when not available).
void setDisplay(HalDisplay* display);
bool beginFrameUpload(int slot);
uint8_t* frameSlot(int slot);          // nullptr when slot not ready
bool frameUploadComplete(int slot);    // size matched
// Bridge chunk path: append one chunk to the active upload slot.
bool frameChunkAppend(const uint8_t* data, size_t len);
void endFrameUpload(int slot);
void swapSlots();
// SD-backed frames: run reads prev/next from SD instead of uploaded slots.
bool setSdFrames(const char* prevPath, const char* nextPath);
// Establish the physical baseline: FULL-refresh the panel to the given SD
// frame so subsequent FAST differential runs start from a known state.
bool baselineFromSd(const char* framePath);
// On-device computed wipe animation: read prev/next from SD into PSRAM, then
// synthesize `steps` intermediate frames (new page enters from the right,
// feather px dithered edge) and refresh each with the current LUT.
// Returns total ms, or 0 on failure. steps==0 disables animation.
uint32_t runAnimate(const char* prevPath, const char* nextPath, int steps, int feather);
bool setLut(const uint8_t* lut, size_t len, bool unlockVoltages);
uint32_t runRefresh(bool swapAfter);   // returns last run ms (0 on failure)
void clearAll();                       // safe full refresh + reset state
Stats stats();

}  // namespace M4WaveformLab
