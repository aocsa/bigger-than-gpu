// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_generate_print.cpp
/// @brief Phase 2.7 Stage F4: Generate (synthetic source) + Print (CSV sink).

#include <gtest/gtest.h>

#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include "quest/cache/data_block.hpp"
#include "quest/taskflow/operators/generate_operator.hpp"
#include "quest/taskflow/operators/print_operator.hpp"
#include "quest/taskflow/pipeline.hpp"
#include "quest/taskflow/pipeline_executor.hpp"
#include "quest/tests/utils.hpp"

using namespace quest;
using namespace quest::taskflow;
using namespace quest::testing;

// =============================================================================
// GenerateOperator
// =============================================================================

TEST(GenerateOperatorTest, EmitsConfiguredBatchesThenEos) {
  GenerateOperator gen{0, /*batch_count=*/3, /*rows_per_batch=*/4};
  EXPECT_TRUE(gen.IsSource());
  EXPECT_FALSE(gen.can_you_throttle_my_input());
  EXPECT_EQ(gen.schema().num_fields(), 1U);

  for (int i = 0; i < 3; ++i) {
    ASSERT_OK_AND_ASSIGN(auto batch, gen.GetNextBatch());
    ASSERT_TRUE(batch.has_value());
    EXPECT_EQ((*batch)->num_rows(), 4U);
  }
  // Exhausted → engaged std::nullopt (EOS is a value, not an error).
  ASSERT_OK_AND_ASSIGN(auto eos, gen.GetNextBatch());
  EXPECT_FALSE(eos.has_value());
}

TEST(GenerateOperatorTest, MultiColumnSchema) {
  GenerateOperator gen{0, /*batch_count=*/2, /*rows_per_batch=*/5, /*num_columns=*/3};
  EXPECT_EQ(gen.schema().num_fields(), 3U);

  ASSERT_OK_AND_ASSIGN(auto batch, gen.GetNextBatch());
  ASSERT_TRUE(batch.has_value());
  auto host = std::dynamic_pointer_cast<cache::HostMemoryBlock>(*batch);
  ASSERT_NE(host, nullptr);
  ASSERT_NE(host->record_batch(), nullptr);
  EXPECT_EQ(host->record_batch()->num_rows(), 5U);
  EXPECT_EQ(host->record_batch()->num_columns(), 3U);
}

// =============================================================================
// PrintOperator
// =============================================================================

TEST(PrintOperatorTest, CapturesCsv) {
  std::ostringstream out;
  PrintOperator print{0, out};
  EXPECT_TRUE(print.IsSink());

  auto block = std::make_shared<cache::HostMemoryBlock>("x", MakeTestRecordBatch(3));
  ASSERT_OK(print.Sink(block));

  EXPECT_EQ(print.total_rows(), 3U);
  EXPECT_EQ(print.batch_count(), 1U);
  EXPECT_FALSE(out.str().empty());
}

TEST(PrintOperatorTest, IgnoresEmptyBatches) {
  std::ostringstream out;
  PrintOperator print{0, out};
  ASSERT_OK(print.Sink(nullptr));
  EXPECT_EQ(print.batch_count(), 0U);
  EXPECT_TRUE(out.str().empty());
}

// =============================================================================
// Generate → Print pipeline
// =============================================================================

TEST(GeneratePrintPipelineTest, GenerateFeedsPrint) {
  GenerateOperator gen{0, /*batch_count=*/3, /*rows_per_batch=*/4};
  std::ostringstream out;
  PrintOperator print{1, out};

  auto pipeline = std::make_unique<Pipeline>(0);
  pipeline->PrependOperator(&print);
  pipeline->PrependOperator(&gen);

  std::vector<PipelinePtr> pipelines;
  pipelines.push_back(std::move(pipeline));

  PipelineExecutor executor{1};
  ASSERT_OK(executor.Execute(std::move(pipelines)));

  EXPECT_EQ(print.total_rows(), 12U);  // 3 batches * 4 rows
  EXPECT_EQ(print.batch_count(), 3U);
  EXPECT_FALSE(out.str().empty());
}
