// ASan canary: writes one byte past the end of a heap buffer ON PURPOSE.
//
// Built only in AddressSanitizer builds. Its ctest entry passes only if ASan's
// heap-buffer-overflow report appears in the output. That proves the sanitizer
// is really compiled in and linked: if a build change ever dropped the ASan
// flags, every "ASan clean" result would silently mean nothing, and this test
// would fail instead.
#include <cstddef>
#include <cstdio>
#include <vector>

int main(int argc, char** /*argv*/) {
  std::vector<char> buffer(16);
  // argc is 1 under ctest, so index == 16 (one past the end). Deriving it from
  // argc keeps the compiler from proving the write is out of bounds at compile
  // time, and the volatile pointer keeps it from deleting the store.
  const std::size_t index = std::size_t{15} + static_cast<std::size_t>(argc);
  volatile char* bytes = buffer.data();
  bytes[index] = 'x';
  std::puts("FAIL: AddressSanitizer did not report the out-of-bounds write");
  return 0;
}
