#include "debug/M4HeapAudit.h"

#if defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include <atomic>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace M4HeapAudit {
namespace {

std::atomic<bool> gHookInstalled{false};

void allocFailedHook(size_t requestedSize, uint32_t caps, const char* functionName) {
  const uint32_t internalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  const uint32_t psramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  // ROM printf avoids heap allocation inside the allocation-failure callback.
  esp_rom_printf("[M4MEM] alloc_fail fn=%s req=%u caps=0x%08x ifree=%u ilargest=%u pfree=%u plargest=%u\n",
                 functionName ? functionName : "?", static_cast<unsigned>(requestedSize),
                 static_cast<unsigned>(caps),
                 static_cast<unsigned>(heap_caps_get_free_size(internalCaps)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(internalCaps)),
                 static_cast<unsigned>(heap_caps_get_free_size(psramCaps)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(psramCaps)));
}

}  // namespace

void installFailedAllocHook() {
  bool expected = false;
  if (!gHookInstalled.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
  const esp_err_t err = heap_caps_register_failed_alloc_callback(allocFailedHook);
  Serial.printf("[M4MEM] alloc_hook err=%d\n", static_cast<int>(err));
}

void snapshot(const char* tag) {
  const uint32_t internalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  const uint32_t psramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  Serial.printf("[M4MEM] tag=%s ifree=%u ilargest=%u imin=%u pfree=%u plargest=%u pmin=%u\n",
                tag ? tag : "?",
                static_cast<unsigned>(heap_caps_get_free_size(internalCaps)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(internalCaps)),
                static_cast<unsigned>(heap_caps_get_minimum_free_size(internalCaps)),
                static_cast<unsigned>(heap_caps_get_free_size(psramCaps)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(psramCaps)),
                static_cast<unsigned>(heap_caps_get_minimum_free_size(psramCaps)));
}

void currentTaskStack(const char* tag) {
#if defined(INCLUDE_uxTaskGetStackHighWaterMark) && INCLUDE_uxTaskGetStackHighWaterMark
  const UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
  Serial.printf("[M4MEM] stack tag=%s high_water=%u\n", tag ? tag : "?",
                static_cast<unsigned>(watermark));
#else
  Serial.printf("[M4MEM] stack tag=%s high_water=disabled\n", tag ? tag : "?");
#endif
}

}  // namespace M4HeapAudit

#else

namespace M4HeapAudit {
void installFailedAllocHook() {}
void snapshot(const char*) {}
void currentTaskStack(const char*) {}
}  // namespace M4HeapAudit

#endif
