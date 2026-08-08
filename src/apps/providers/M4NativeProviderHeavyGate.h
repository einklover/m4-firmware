#pragma once

#include <mutex>

namespace M4NativeProviderHeavyGate {

// One process-wide gate for TLS/decode jobs. ESP32-S3 internal RAM is the
// scarce resource even when payload buffers live in PSRAM; two simultaneous
// handshakes can fragment/starve the internal heap and turn a safe streaming
// path into an OOM. Discovery/catalog/chapter workers all take this gate.
inline std::mutex& mutex() {
  static std::mutex g;
  return g;
}

using Lock = std::unique_lock<std::mutex>;

}  // namespace M4NativeProviderHeavyGate
