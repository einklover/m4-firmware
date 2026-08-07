#pragma once

namespace M4HeapAudit {

// Register ESP-IDF's allocation-failure hook once. No-op off ESP32.
void installFailedAllocHook();

// Print internal/PSRAM free, largest block and minimum-free watermarks.
void snapshot(const char* tag);

// Print the current task's stack high-water mark (ESP-IDF reports bytes).
void currentTaskStack(const char* tag);

}  // namespace M4HeapAudit
