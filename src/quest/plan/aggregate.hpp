// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file aggregate.hpp
/// @brief Aggregate function kinds and the SUM typing rule, shared by the logical plan and
/// HashAggregateOperator.

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "quest/columnar/types.hpp"

namespace quest::plan {

/// @brief Aggregate function of an `AggregateExpr` / `AggregateSpec`.
enum class AggregateKind : std::uint8_t { kSum, kCount, kCountStar, kMin, kMax, kAvg };

inline constexpr std::array<std::string_view, 6> kAggregateNames = {"SUM", "COUNT", "COUNT",
                                                                     "MIN", "MAX",   "AVG"};

/// @brief SQL name of @p kind; COUNT(*) is "COUNT".
[[nodiscard]] constexpr std::string_view ToString(AggregateKind kind) noexcept {
  return kAggregateNames[std::to_underlying(kind)];
}

/// @brief The SQL type of SUM over @p input, as DuckDB types it: Int64 for Bool and the
/// integers, Double for Float and Double, Decimal(18, s) for a Decimal (DuckDB widens to 38
/// digits; QUEST keeps an int64 and fails on overflow).
/// @param input The input column type.
/// @return The SUM type, or nullopt for a non-numeric input.
[[nodiscard]] constexpr std::optional<columnar::DataType> SumType(columnar::DataType input) noexcept {
  if (columnar::IsDecimal(input)) {
    return columnar::DataType::Decimal(columnar::DataType::kMaxDecimalPrecision, input.scale);
  }
  if (input == columnar::DataType::kBool || columnar::IsInteger(input)) {
    return columnar::DataType::kInt64;
  }
  if (columnar::IsNumeric(input)) {
    return columnar::DataType::kDouble;
  }
  return std::nullopt;
}

}  // namespace quest::plan
