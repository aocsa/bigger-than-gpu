// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0
#ifndef QUEST_EXECUTION_BLOOM_FILTER_HPP_
#define QUEST_EXECUTION_BLOOM_FILTER_HPP_

/// @file bloom_filter.hpp
/// @brief Minimal insert-only Bloom filter for integer equality keys.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "quest/result.hpp"

namespace quest::execution {

// =============================================================================
// Integer membership filter (single writer, immutable during concurrent reads)
// =============================================================================

/// @brief Approximate membership: false proves absence; true requires an exact check.
/// No deletion or concurrent mutation is supported. All allocated words start at zero.
class BloomFilter {
 public:
  /// @brief Create a filter with a power-of-two bit count and 1..16 probes per key.
  /// @param bit_count Number of bits, at least 64 and at most 2^30 (128 MiB).
  /// @param hash_count Number of bit positions tested for each key.
  /// @return Filter, or Invalid for unsupported configuration.
  [[nodiscard]] static Result<BloomFilter> Create(std::size_t bit_count = 1024,
                                                 std::size_t hash_count = 7) {
    if (bit_count < 64 || bit_count > (std::size_t{1} << 30) ||
        (bit_count & (bit_count - 1)) != 0 || hash_count == 0 || hash_count > 16) {
      return Status::Invalid("Bloom requires power-of-two bits in [64, 2^30] and hashes in [1, 16]");
    }
    return BloomFilter(bit_count, hash_count);
  }

  /// @brief Insert a key; repeated insertion is harmless.
  /// @param key Integer key in the same representation used for lookup.
  void Insert(uint64_t key) noexcept {
    auto position = Mix(key);
    const auto step = Mix(key ^ kSeed) | uint64_t{1};
    for (std::size_t i = 0; i < hash_count_; ++i, position += step) {
      const auto bit = position & bit_mask_;
      words_[bit / 64] |= uint64_t{1} << (bit % 64);
    }
  }

  /// @brief Check membership without modifying the filter.
  /// @param key Integer key to look up.
  /// @return False if absent, true if possibly present.
  [[nodiscard]] bool MayContain(uint64_t key) const noexcept {
    auto position = Mix(key);
    const auto step = Mix(key ^ kSeed) | uint64_t{1};
    for (std::size_t i = 0; i < hash_count_; ++i, position += step) {
      const auto bit = position & bit_mask_;
      if ((words_[bit / 64] & (uint64_t{1} << (bit % 64))) == 0) {
        return false;
      }
    }
    return true;
  }

  /// @brief Report the size of the bit array.
  /// @return Allocated bytes, excluding vector metadata.
  [[nodiscard]] std::size_t GetSizeBytes() const noexcept { return words_.size() * 8; }

 private:
  BloomFilter(std::size_t bit_count, std::size_t hash_count)
      : words_(bit_count / 64, 0), bit_mask_(bit_count - 1), hash_count_(hash_count) {}

  // SplitMix64 finalizer. Unsigned overflow is intentional and well-defined.
  static uint64_t Mix(uint64_t value) noexcept {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
  }

  static constexpr uint64_t kSeed = UINT64_C(0x9e3779b97f4a7c15);
  std::vector<uint64_t> words_;
  uint64_t bit_mask_;
  std::size_t hash_count_;
};

}  // namespace quest::execution
#endif  // QUEST_EXECUTION_BLOOM_FILTER_HPP_
