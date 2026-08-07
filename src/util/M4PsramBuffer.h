#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

// Small PSRAM-first primitives for CPU-only scratch buffers.
//
// Do not use these for DMA/SPI/SD driver transfer buffers. Callers that feed a
// DMA-capable peripheral should keep a bounded MALLOC_CAP_INTERNAL|DMA bounce
// buffer and copy to/from this storage.
namespace M4Psram {

inline void* alloc(size_t bytes) {
  if (bytes == 0) return nullptr;
#if defined(ARDUINO_ARCH_ESP32)
  void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  return p;
#else
  return std::malloc(bytes);
#endif
}

inline void release(void* ptr) {
  if (!ptr) return;
#if defined(ARDUINO_ARCH_ESP32)
  heap_caps_free(ptr);
#else
  std::free(ptr);
#endif
}

template <typename T>
class Buffer {
  static_assert(std::is_trivially_copyable<T>::value,
                "M4Psram::Buffer is intended for POD/trivially-copyable scratch data");

 public:
  Buffer() = default;
  explicit Buffer(size_t count) { (void)resize(count); }
  ~Buffer() { reset(); }

  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;

  Buffer(Buffer&& other) noexcept
      : data_(other.data_), size_(other.size_), capacity_(other.capacity_) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
  }

  Buffer& operator=(Buffer&& other) noexcept {
    if (this == &other) return *this;
    reset();
    data_ = other.data_;
    size_ = other.size_;
    capacity_ = other.capacity_;
    other.data_ = nullptr;
    other.size_ = 0;
    other.capacity_ = 0;
    return *this;
  }

  bool resize(size_t count) {
    if (count <= capacity_) {
      size_ = count;
      return true;
    }
    if (count > std::numeric_limits<size_t>::max() / sizeof(T)) return false;

    T* next = static_cast<T*>(alloc(count * sizeof(T)));
    if (!next) return false;
    if (data_ && size_ > 0) {
      const size_t keep = size_ < count ? size_ : count;
      std::memcpy(next, data_, keep * sizeof(T));
    }
    release(data_);
    data_ = next;
    size_ = count;
    capacity_ = count;
    return true;
  }

  void reset() {
    release(data_);
    data_ = nullptr;
    size_ = 0;
    capacity_ = 0;
  }

  T* data() { return data_; }
  const T* data() const { return data_; }
  size_t size() const { return size_; }
  size_t capacity() const { return capacity_; }
  bool empty() const { return size_ == 0; }

  T& operator[](size_t i) { return data_[i]; }
  const T& operator[](size_t i) const { return data_[i]; }

 private:
  T* data_ = nullptr;
  size_t size_ = 0;
  size_t capacity_ = 0;
};

}  // namespace M4Psram
