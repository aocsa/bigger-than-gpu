// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file print_operator.hpp
/// @brief Debug sink that writes each batch as CSV to an injectable ostream.
///
/// A terminal sink (IsSink() == true): every batch that reaches it is
/// materialized on CPU and its `RecordBatch::ToCSV()` written to the ostream the
/// caller supplied — the pipeline analog of a `print` statement. The ostream is
/// injected (not hard-wired to std::cout) so tests capture and assert the output.

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <utility>

#include "quest/cache/data_block.hpp"
#include "quest/common/result.hpp"
#include "quest/taskflow/operator.hpp"

namespace quest::taskflow {

/// @brief Terminal debug sink: prints each received batch as CSV.
class PrintOperator : public Operator {
 public:
  /// @param out Destination stream (must outlive this operator).
  PrintOperator(std::uint32_t operator_id, std::ostream& out)
      : Operator(operator_id, "Print", OperatorType::Print), out_(out) {}

  [[nodiscard]] bool IsSink() const noexcept override { return true; }

  [[nodiscard]] Status Sink(cache::DataBlockPtr batch) override {
    if (!batch || batch->num_rows() == 0) {
      return {};
    }
    auto host = cache::EnsureHostBlock(std::move(batch));
    if (!host) {
      return std::unexpected(host.error());
    }
    const auto* record_batch = (*host)->record_batch();
    out_ << record_batch->ToCSV();
    total_rows_ += record_batch->num_rows();
    ++batch_count_;
    return {};
  }

  /// @brief Total rows printed (valid after Execute() returns).
  [[nodiscard]] std::size_t total_rows() const noexcept { return total_rows_; }

  [[nodiscard]] std::size_t batch_count() const noexcept { return batch_count_; }

 private:
  std::ostream& out_;
  std::size_t total_rows_{0};
  std::size_t batch_count_{0};
};

}  // namespace quest::taskflow
