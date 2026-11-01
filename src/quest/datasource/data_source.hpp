// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "quest/columnar/record_batch.hpp"
#include "quest/columnar/schema.hpp"
#include "quest/common/result.hpp"

namespace quest::datasource {

// =============================================================================
// Abstract Data Source
// =============================================================================

class DataSource {
 public:
  virtual ~DataSource() = default;

  [[nodiscard]] virtual Schema schema() const = 0;

  /// Independent units of scan work (Parquet: row groups).
  [[nodiscard]] virtual std::size_t num_partitions() const = 0;

  /// @param projection Columns to read; empty reads all.
  [[nodiscard]] virtual Result<RecordBatch> ReadPartition(
      std::size_t index, const std::vector<std::string>& projection) = 0;
};

}  // namespace quest::datasource
