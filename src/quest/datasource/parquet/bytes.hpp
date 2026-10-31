// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file bytes.hpp
/// @brief Bounds-checked reads from Parquet byte buffers.
///
/// Every length and offset in a Parquet page comes from the file, so a corrupt or truncated
/// file must surface as an error instead of an out-of-bounds read.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <type_traits>

#include "quest/result.hpp"

namespace quest::datasource::parquet {

/// @brief Returns `data[pos, pos + len)`, or OutOfRange if it does not fit.
[[nodiscard]] inline Result<std::span<const uint8_t>> Slice(std::span<const uint8_t> data,
                                                            std::size_t pos, std::size_t len) {
  if (pos > data.size() || len > data.size() - pos) {
    return Error::OutOfRange(
        std::format("read of {} bytes at offset {} exceeds buffer of {}", len, pos, data.size()));
  }
  return data.subspan(pos, len);
}

/// @brief Reads a little-endian `T` at `pos`, or OutOfRange if it does not fit.
template <typename T>
  requires std::is_trivially_copyable_v<T>
[[nodiscard]] Result<T> LoadLE(std::span<const uint8_t> data, std::size_t pos) {
  auto bytes = Slice(data, pos, sizeof(T));
  if (!bytes) {
    return std::unexpected(std::move(bytes).error());
  }
  std::array<uint8_t, sizeof(T)> raw;
  std::ranges::copy(*bytes, raw.begin());
  if constexpr (std::endian::native == std::endian::big) {
    std::ranges::reverse(raw);
  }
  return std::bit_cast<T>(raw);
}

}  // namespace quest::datasource::parquet
