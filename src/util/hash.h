// MurmurHash3 x64_128: a fast, well-mixed, non-cryptographic hash.
//
// Used by the bloom filters (its two 64-bit halves are the two bases of double
// hashing) and, later, to shard the block cache. Its output is written into
// table files through the filters, so this function is part of the file
// format and must never change (docs/file-formats.md, "Bloom filter").
//
// Written from Austin Appleby's public-domain reference, MurmurHash3.cpp in
// https://github.com/aappleby/smhasher (function MurmurHash3_x64_128). The
// unit tests compare it against an independent implementation's output.
#pragma once

#include <cstdint>
#include <string_view>

namespace forgedb {

struct Hash128 {
  std::uint64_t low = 0;   // the reference's out[0] (h1)
  std::uint64_t high = 0;  // the reference's out[1] (h2)

  friend bool operator==(const Hash128&, const Hash128&) = default;
};

// The reference's MurmurHash3_x64_128(data, size, seed, out). Reads input
// bytes as little-endian 64-bit words, as the reference does on x64.
[[nodiscard]] Hash128 MurmurHash3x64(std::string_view data, std::uint32_t seed = 0) noexcept;

}  // namespace forgedb
