// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_manual_query_walkthrough.cpp
/// @brief Tutorial test: recreates
///
///   FromParquet(f).Where(Col("salary") > Lit(50000.0))
///                 .Select({"id","name","salary"})
///                 .ExecuteOnGraph()
///
/// entirely from internal APIs, layer by layer, so reading it top to
/// bottom teaches how the components connect:
///
///   Test 1 (RolesOnlyNoEngine)     — just the four operators and their
///     pipeline roles, driven by a hand-written loop. No Graph, no
///     Pipeline, no executor, no threads.
///   Test 2 (ManualAssemblyEndToEnd) — every layer explicit: logical plan
///     -> physical expressions -> operators -> Graph wiring ->
///     MetaPipeline::Build (shape asserted) -> PipelineExecutor ->
///     collection, checked against the real fluent API and known row counts.
///
/// Nothing here is the recommended way to run a query — that is exactly
/// one line of QueryBuilder. This file exists to show what that line does.

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "quest/builder/query_builder.hpp"        // the oracle (fluent API)
#include "quest/datasource/data_source.hpp"       // ParquetDataSource
#include "quest/execution/graph_builder.hpp"      // CreatePhysicalExpr
#include "quest/logical_plan/logical_expr.hpp"    // col(), lit(), gt()
#include "quest/logical_plan/logical_plan.hpp"    // Scan, Selection, Projection
#include "quest/taskflow/graph_builder.hpp"       // GraphBuilder (Phase 2.5 D3)
#include "quest/taskflow/meta_pipeline.hpp"       // MetaPipeline::Build
#include "quest/taskflow/operators/collector_operator.hpp"
#include "quest/taskflow/operators/filter_operator.hpp"
#include "quest/taskflow/operators/project_operator.hpp"
#include "quest/taskflow/operators/table_scan_operator.hpp"
#include "quest/taskflow/pipeline_executor.hpp"  // PipelineExecutor
#include "support/test_utils.hpp"

using namespace quest;
using namespace quest::taskflow;
using namespace quest::datasource;

namespace {

[[nodiscard]] std::filesystem::path EmployeesParquet() {
  return std::filesystem::path(__FILE__).parent_path().parent_path() / "examples" /
         "employees_uncompressed.parquet";
}

constexpr double kSalaryThreshold = 50000.0;
constexpr std::size_t kRowsAboveThreshold = 90966;  // of 100000 rows in the file
const std::vector<std::string> kSelectedColumns = {"id", "name", "salary"};

}  // namespace

// =============================================================================
// Test 1 — the operators alone. This is Pipeline::RunTask written by hand:
// the pipeline driver does nothing more than this loop.
//
//   source role:  scan.GetNextBatch()      DataSource -> tracked block
//   execute role: filter.ProcessBatch(b)   predicate mask + gather
//   execute role: project.ProcessBatch(b)  expr eval -> projected schema
//   sink role:    collector.Sink(b)        accumulate results
// =============================================================================

TEST(ManualQueryWalkthroughTest, RolesOnlyNoEngine) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }

  // --- Ingredients ---------------------------------------------------------
  // A DataSource for the scan, and a memory manager: every operator stamps
  // its output blocks with it so allocations are tracked and (at pipeline
  // breaks) spillable. Test 1 never crosses a break, but the contract is
  // uniform: operators don't know where their outputs will travel.
  ASSERT_OK_AND_ASSIGN(auto opened, ParquetDataSource::Open(EmployeesParquet().string()));
  auto data_source = std::make_shared<ParquetDataSource>(std::move(opened));
  auto memory_manager = make_default_memory_manager();

  // --- Physical expressions ------------------------------------------------
  // Operators consume PHYSICAL expressions (column indexes, typed eval),
  // not logical ones (column names). CreatePhysicalExpr resolves names
  // against the input plan's schema — so we need a logical Scan node as
  // the schema provider even in this engine-less test.
  auto scan_node = std::make_unique<Scan>(EmployeesParquet().string(), data_source);

  auto predicate_logical = gt(col("salary"), lit(kSalaryThreshold));
  ASSERT_OK_AND_ASSIGN(auto predicate, CreatePhysicalExpr(*predicate_logical, *scan_node));

  std::vector<ExpressionPtr> projection_exprs;
  std::vector<Field> projection_fields;
  for (const auto& name : kSelectedColumns) {
    auto column_logical = col(name);
    ASSERT_OK_AND_ASSIGN(auto column_phys, CreatePhysicalExpr(*column_logical, *scan_node));
    projection_exprs.push_back(std::move(column_phys));
    ASSERT_OK_AND_ASSIGN(auto field, column_logical->ToField(*scan_node));
    projection_fields.push_back(field);
  }

  TableScanOperator scan{0, data_source, std::nullopt, memory_manager};
  FilterOperator filter{1, std::move(predicate), memory_manager};
  ProjectOperator project{2, Schema(projection_fields), std::move(projection_exprs),
                          memory_manager};
  CollectorOperator collector{3};

  // ProcessBatch is public on Operator (the driver's view) but protected
  // on SimpleOperator subclasses (implementers override it, they don't
  // call it) — so we drive through the base, exactly like RunTask does.
  Operator& filter_role = filter;
  Operator& project_role = project;

  // --- The hand-written push loop ------------------------------------------
  std::size_t batches_seen = 0;
  while (true) {
    // Source role: EOS is a VALUE — an engaged optional holding nullopt —
    // never an error code.
    ASSERT_OK_AND_ASSIGN(auto maybe_batch, scan.GetNextBatch());
    if (!maybe_batch.has_value()) {
      break;  // end of stream
    }
    ++batches_seen;

    // Execute role, in plan order. A filter may consume the whole batch;
    // RunTask short-circuits the same way.
    ASSERT_OK_AND_ASSIGN(auto filtered, filter_role.ProcessBatch(std::move(*maybe_batch)));
    if (!filtered || filtered->num_rows() == 0) {
      continue;
    }
    ASSERT_OK_AND_ASSIGN(auto projected, project_role.ProcessBatch(std::move(filtered)));

    ASSERT_OK(collector.Sink(std::move(projected)));
  }

  EXPECT_GT(batches_seen, 0U);
  EXPECT_GT(collector.total_rows(), 0U);
  EXPECT_EQ(collector.total_rows(), kRowsAboveThreshold);

  // Same rows as the real query — the loop above IS the query.
  ASSERT_OK_AND_ASSIGN(auto oracle, QueryBuilder::FromParquet(EmployeesParquet().string())
                                        .value()
                                        .Where(Col("salary") > Lit(kSalaryThreshold))
                                        .Select({"id", "name", "salary"})
                                        .ExecuteOnGraph());
  std::size_t oracle_rows = 0;
  for (const auto& batch : oracle) {
    oracle_rows += batch.num_rows();
  }
  EXPECT_EQ(collector.total_rows(), oracle_rows);
}

// =============================================================================
// Test 2 — every layer explicit. Each stage is exactly what one internal
// component does for you; the stage comments name that component.
// =============================================================================

TEST(ManualQueryWalkthroughTest, ManualAssemblyEndToEnd) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }
  const auto path = EmployeesParquet().string();

  // ==========================================================================
  // Stage 1 — the logical plan, by hand.
  // (What QueryBuilder's fluent chain builds: FromParquet -> Scan node,
  //  .Where -> Selection node, .Select -> Projection node. ExprBuilder's
  //  operator overloads are sugar over the col()/lit()/gt() factories.)
  // ==========================================================================
  ASSERT_OK_AND_ASSIGN(auto opened, ParquetDataSource::Open(path));
  auto data_source = std::make_shared<ParquetDataSource>(std::move(opened));

  auto scan_node = std::make_unique<Scan>(path, data_source);

  auto selection_node =
      std::make_unique<Selection>(std::move(scan_node), gt(col("salary"), lit(kSalaryThreshold)));

  std::vector<LogicalExprPtr> select_list;
  for (const auto& name : kSelectedColumns) {
    select_list.push_back(col(name));
  }
  auto projection_node =
      std::make_unique<Projection>(std::move(selection_node), std::move(select_list));

  // ==========================================================================
  // Stage 2 — physical expressions + output schema, by hand.
  // (What execution::CreateGraph's TranslateChain does per node: resolve
  //  each logical expression against its INPUT plan's schema. The
  //  Selection's predicate resolves against the Scan; the Projection's
  //  columns resolve against the Selection.)
  // ==========================================================================
  const auto* selection = dynamic_cast<const Selection*>(&projection_node->input());
  ASSERT_NE(selection, nullptr);

  ASSERT_OK_AND_ASSIGN(auto predicate,
                       CreatePhysicalExpr(selection->condition(), selection->input()));

  std::vector<ExpressionPtr> projection_exprs;
  std::vector<Field> projection_fields;
  for (const auto& expr : projection_node->expressions()) {
    ASSERT_OK_AND_ASSIGN(auto phys, CreatePhysicalExpr(*expr, projection_node->input()));
    projection_exprs.push_back(std::move(phys));
    ASSERT_OK_AND_ASSIGN(auto field, expr->ToField(projection_node->input()));
    projection_fields.push_back(field);
  }

  // ==========================================================================
  // Stage 3 — the operators, by hand.
  // (What CreateGraph constructs. Every operator takes the memory manager
  //  so its output blocks are tracked + spillable — MakeTrackedHostBlock.)
  // ==========================================================================
  auto memory_manager = make_default_memory_manager();

  TableScanOperator scan_op{0, data_source, std::nullopt, memory_manager};
  FilterOperator filter_op{1, std::move(predicate), memory_manager};
  ProjectOperator project_op{2, Schema(projection_fields), std::move(projection_exprs),
                             memory_manager};
  CollectorOperator collector_op{3};

  // ==========================================================================
  // Stage 4 — the graph wiring, by hand.
  // (CreateGraph's edge policy: streaming edges are plain — they will
  //  FUSE into one pipeline — but the collector sits behind a Full
  //  barrier, Sirius's RESULT_COLLECTOR shape. CollectorOperator declares
  //  Full via InputEdgeContract, so GraphBuilder infers it automatically.)
  // ==========================================================================
  GraphBuilder gb{memory_manager};
  gb.Chain(scan_op, filter_op, project_op, collector_op);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());
  graph->Materialize();  // holders deferred from AddEdge until Prepare/Materialize

  // ==========================================================================
  // Stage 5 — pipelines, made visible.
  // (Graph::Execute calls this internally; doing it by hand lets
  //  us LOOK at the structure: the streaming chain fused into one
  //  pipeline, the collector alone in a second one, gated FULL.)
  // ==========================================================================
  ASSERT_OK_AND_ASSIGN(auto pipelines, MetaPipeline::Build(*graph));

  ASSERT_EQ(pipelines.size(), 2U);
  const auto& producer = pipelines[0];  // children before parents, always
  const auto& consumer = pipelines[1];

  EXPECT_EQ(producer->operators(), (std::vector<Operator*>{&scan_op, &filter_op, &project_op}));
  EXPECT_TRUE(producer->source()->IsSource());  // scan has the source role

  EXPECT_EQ(consumer->operators(), (std::vector<Operator*>{&collector_op}));
  EXPECT_TRUE(consumer->sink()->IsSink());  // collector has the sink role
  ASSERT_EQ(consumer->FullBarrierProducers().size(), 1U);
  EXPECT_EQ(consumer->FullBarrierProducers()[0], producer.get());

  // ==========================================================================
  // Stage 6 — execution, one layer below Graph::Execute().
  // (The executor seeds its ready queue with pipelines whose Full
  //  producers are all finished — just the producer here. It drains it
  //  batch-by-batch with the push loop from Test 1, publishes across the
  //  holder, and only then does the consumer pipeline become runnable.)
  // ==========================================================================
  PipelineExecutor executor{2};
  ASSERT_OK(executor.Execute(std::move(pipelines)));

  ASSERT_OK_AND_ASSIGN(auto manual_batches, collector_op.CollectBatches());

  // ==========================================================================
  // Oracle — the fluent one-liner this whole file deconstructs.
  // ==========================================================================
  ASSERT_OK_AND_ASSIGN(auto oracle_batches, QueryBuilder::FromParquet(path)
                                                .value()
                                                .Where(Col("salary") > Lit(kSalaryThreshold))
                                                .Select({"id", "name", "salary"})
                                                .ExecuteOnGraph());

  std::size_t manual_rows = 0;
  for (const auto& batch : manual_batches) {
    manual_rows += batch.num_rows();
  }
  EXPECT_EQ(manual_rows, kRowsAboveThreshold);

  ASSERT_EQ(manual_batches.size(), oracle_batches.size());
  for (std::size_t i = 0; i < manual_batches.size(); ++i) {
    EXPECT_EQ(manual_batches[i].ToCSV(), oracle_batches[i].ToCSV())
        << "batch " << i << " diverged from the fluent API";
  }
}
