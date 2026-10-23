// Move-only owners of Win32 handles (RAII).
//
// The handle is closed exactly once: when the owner is destroyed, reset, or
// assigned another handle. No error path can leak it, and it can't be closed
// twice because copies are impossible.
//
// Windows-only, and included only by src/env/ (and Windows-specific tests).
#pragma once

#include <windows.h>

#include <utility>

namespace forgedb {

// How to recognize an invalid handle and how to close a valid one.
// CreateFileW returns INVALID_HANDLE_VALUE on failure; other APIs return
// nullptr, so both count as "no handle".
struct KernelHandleTraits {
  static void Close(HANDLE handle) noexcept { ::CloseHandle(handle); }
};
struct FindHandleTraits {
  static void Close(HANDLE handle) noexcept { ::FindClose(handle); }
};

template <typename Traits>
class BasicUniqueHandle {
 public:
  BasicUniqueHandle() noexcept = default;
  explicit BasicUniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
  ~BasicUniqueHandle() { Reset(); }

  BasicUniqueHandle(const BasicUniqueHandle&) = delete;
  BasicUniqueHandle& operator=(const BasicUniqueHandle&) = delete;

  BasicUniqueHandle(BasicUniqueHandle&& other) noexcept : handle_(other.Release()) {}
  BasicUniqueHandle& operator=(BasicUniqueHandle&& other) noexcept {
    if (this != &other) {
      Reset(other.Release());
    }
    return *this;
  }

  [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
  [[nodiscard]] bool IsValid() const noexcept { return IsValidHandle(handle_); }

  // Gives up ownership without closing; the caller must close the handle.
  [[nodiscard]] HANDLE Release() noexcept { return std::exchange(handle_, INVALID_HANDLE_VALUE); }

  // Closes the current handle (if any) and takes ownership of `handle`.
  void Reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept {
    HANDLE old = std::exchange(handle_, handle);
    if (IsValidHandle(old)) {
      Traits::Close(old);
    }
  }

 private:
  static bool IsValidHandle(HANDLE handle) noexcept {
    return handle != INVALID_HANDLE_VALUE && handle != nullptr;
  }

  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

using UniqueHandle = BasicUniqueHandle<KernelHandleTraits>;    // CloseHandle
using UniqueFindHandle = BasicUniqueHandle<FindHandleTraits>;  // FindClose

}  // namespace forgedb
