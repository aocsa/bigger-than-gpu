/// @file simple_graph.cpp
/// @brief Example demonstrating the taskflow DSL for a simple query pipeline

#include <format>
#include <iostream>
#include <quest/quest.hpp>
#include <quest/taskflow/graph.hpp>

#include "quest/cache/data_block.hpp"
#include "quest/datasource/data_source.hpp"
#include "quest/logical_plan/logical_expr.hpp"
#include "quest/logical_plan/logical_plan.hpp"
#include "quest/physical_plan/expressions/physical_expr.hpp"
using namespace quest;
using namespace quest::taskflow;
using namespace quest::cache;
using namespace quest::datasource::parquet;

// =============================================================================
// Example Operators
// =============================================================================

/// @brief Table scan operator that generates sample data
class TableScanOperator : public Operator {
 public:
  TableScanOperator(std::uint32_t id, Scan* scan)
      : Operator(id, std::format("TableScan({})", scan->ToString()), OperatorType::TableScan), scan(scan) {}

  [[nodiscard]] bool can_you_throttle_my_input() const override { return false; }

  [[nodiscard]] ExecutionStatus run() override {
    std::cout << std::format("[{}] Starting scan of '{}'\n", id(), scan->ToString());
    std::vector<std::string> proj;
    for (const auto& field : scan->schema().fields()) {
      proj.push_back(field.name);
    }

    auto sequence = scan->data_source()->Scan(proj);
    int index = 0;
    for (auto iter = sequence.begin(); iter != sequence.end(); ++iter) {
      auto batch_result = *iter;
      if (!batch_result) {
        return ExecutionStatus::Stop;
      }
      auto data_block =
          std::make_shared<HostMemoryBlock>(std::format("batch_{}", index), std::move(*batch_result));
      auto status = AddToOutput(std::move(data_block));
      if (!status.ok()) {
        return ExecutionStatus::Stop;
      }
      ++index;
    }

    std::cout << std::format("[{}] Scan complete\n", id());
    return ExecutionStatus::Proceed;
  }

 private:
  Scan* scan;
};

[[nodiscard]] inline Result<ExpressionPtr> MyCreatePhysicalExpr(const LogicalExpr& expr,
                                                              const Schema& schema) {
  if (const auto* literal = dynamic_cast<const Literal*>(&expr)) {
    return std::make_unique<LiteralExpression>(literal->value());
  }

  if (const auto* col_idx = dynamic_cast<const ColumnIndex*>(&expr)) {
    return std::make_unique<ColumnExpression>(col_idx->index());
  }

  if (const auto* col = dynamic_cast<const Column*>(&expr)) {
    auto index = schema.GetFieldIndex(col->name());
    if (!index) {
      return std::unexpected(Status::SchemaError("No column named: " + col->name()));
    }
    return std::make_unique<ColumnExpression>(*index);
  }

  // Handle binary expressions
  if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
    auto left_result = MyCreatePhysicalExpr(binary->left(), schema);
    if (!left_result) {
      return std::unexpected(left_result.error());
    }
    auto right_result = MyCreatePhysicalExpr(binary->right(), schema);
    if (!right_result) {
      return std::unexpected(right_result.error());
    }

    const auto& op = binary->op();
    if (op == "<") {
      return std::make_unique<LtExpression>(std::move(*left_result), std::move(*right_result));
    } else if (op == "<=") {
      return std::make_unique<LtEqExpression>(std::move(*left_result), std::move(*right_result));
    } else if (op == ">") {
      return std::make_unique<GtExpression>(std::move(*left_result), std::move(*right_result));
    } else if (op == ">=") {
      return std::make_unique<GtEqExpression>(std::move(*left_result), std::move(*right_result));
    } else if (op == "==") {
      return std::make_unique<EqExpression>(std::move(*left_result), std::move(*right_result));
    } else if (op == "!=") {
      return std::make_unique<NeqExpression>(std::move(*left_result), std::move(*right_result));
    } else if (op == "+") {
      return std::make_unique<AddExpression>(std::move(*left_result), std::move(*right_result));
    } else if (op == "-") {
      return std::make_unique<SubtractExpression>(std::move(*left_result), std::move(*right_result));
    } else if (op == "*") {
      return std::make_unique<MultiplyExpression>(std::move(*left_result), std::move(*right_result));
    } else if (op == "/") {
      return std::make_unique<DivideExpression>(std::move(*left_result), std::move(*right_result));
    } else if (op == "%") {
      return std::make_unique<ModuloExpression>(std::move(*left_result), std::move(*right_result));
    } else if (op == "AND") {
      return std::make_unique<AndExpression>(std::move(*left_result), std::move(*right_result));
    } else if (op == "OR") {
      return std::make_unique<OrExpression>(std::move(*left_result), std::move(*right_result));
    }
  }

  return std::unexpected(Status::ExpressionError("Expression type not supported: " + expr.ToString()));
}

std::shared_ptr<datasource::ColumnVector> MyFilterColumn(
      const datasource::ColumnVector& col,
      const std::vector<size_t>& indices) {
    std::vector<Value> filtered_values;
    filtered_values.reserve(indices.size());
    for (size_t idx : indices) {
      filtered_values.push_back(col.GetValue(idx));
    }

    // Create a materialized column with the filtered values
    // Determine the type and create appropriate vector
    return std::visit(
        [&filtered_values](const auto& type_tag) -> std::shared_ptr<datasource::ColumnVector> {
          using TypeTag = std::decay_t<decltype(type_tag)>;

          if constexpr (std::is_same_v<TypeTag, datasource::BoolType>) {
            std::vector<bool> data;
            data.reserve(filtered_values.size());
            for (const auto& val : filtered_values) {
              if (auto* v = std::get_if<bool>(&val)) {
                data.push_back(*v);
              } else {
                data.push_back(false);
              }
            }
            return std::make_shared<MaterializedColumnVector>(
                ColumnData{std::move(data)}, type_tag);
          } else if constexpr (std::is_same_v<TypeTag, datasource::Int32Type>) {
            std::vector<int32_t> data;
            data.reserve(filtered_values.size());
            for (const auto& val : filtered_values) {
              if (auto* v = std::get_if<int32_t>(&val)) {
                data.push_back(*v);
              } else {
                data.push_back(0);
              }
            }
            return std::make_shared<MaterializedColumnVector>(
                ColumnData{std::move(data)}, type_tag);
          } else if constexpr (std::is_same_v<TypeTag, datasource::Int64Type>) {
            std::vector<int64_t> data;
            data.reserve(filtered_values.size());
            for (const auto& val : filtered_values) {
              if (auto* v = std::get_if<int64_t>(&val)) {
                data.push_back(*v);
              } else {
                data.push_back(0);
              }
            }
            return std::make_shared<MaterializedColumnVector>(
                ColumnData{std::move(data)}, type_tag);
          } else if constexpr (std::is_same_v<TypeTag, datasource::FloatType>) {
            std::vector<float> data;
            data.reserve(filtered_values.size());
            for (const auto& val : filtered_values) {
              if (auto* v = std::get_if<float>(&val)) {
                data.push_back(*v);
              } else {
                data.push_back(0.0f);
              }
            }
            return std::make_shared<MaterializedColumnVector>(
                ColumnData{std::move(data)}, type_tag);
          } else if constexpr (std::is_same_v<TypeTag, datasource::DoubleType>) {
            std::vector<double> data;
            data.reserve(filtered_values.size());
            for (const auto& val : filtered_values) {
              if (auto* v = std::get_if<double>(&val)) {
                data.push_back(*v);
              } else {
                data.push_back(0.0);
              }
            }
            return std::make_shared<MaterializedColumnVector>(
                ColumnData{std::move(data)}, type_tag);
          } else if constexpr (std::is_same_v<TypeTag, datasource::StringType>) {
            std::vector<std::string> data;
            data.reserve(filtered_values.size());
            for (const auto& val : filtered_values) {
              if (auto* v = std::get_if<std::string>(&val)) {
                data.push_back(*v);
              } else {
                data.emplace_back();
              }
            }
            return std::make_shared<MaterializedColumnVector>(
                ColumnData{std::move(data)}, type_tag);
          } else {
            // Default: create string column
            std::vector<std::string> data;
            data.reserve(filtered_values.size());
            for (const auto& val : filtered_values) {
              data.push_back(datasource::ValueToString(val));
            }
            return std::make_shared<MaterializedColumnVector>(
                ColumnData{std::move(data)}, datasource::StringType{});
          }
        },
        col.GetType());
}

/// @brief Filter operator that passes through data
class FilterOperator : public SimpleOperator {
 public:
  FilterOperator(std::uint32_t id, Selection* selection)
      : SimpleOperator(id, std::format("Filter({})", selection->ToString()), OperatorType::Filter), selection(selection) {}

 protected:
  [[nodiscard]] Result<std::shared_ptr<DataBlock>> ProcessBatch(std::shared_ptr<DataBlock> input) override {
    std::cout << std::format("[{}] Filtering batch with condition: {}\n", id(),
                             selection->ToString());

    auto expr_result = MyCreatePhysicalExpr(selection->condition(), selection->input().schema());
    if (!expr_result) {
      std::cerr << std::format("[{}] Failed to create physical expression: {}\n", id(), expr_result.error().message_);
      return std::unexpected(expr_result.error());
    }
    auto expr = std::move(*expr_result);

    auto* cpu_result = static_cast<HostMemoryBlock*>(input.get());;
    auto *batch = cpu_result->record_batch();

    auto mask_result = expr->Evaluate(*batch);
    if (!mask_result) {
      std::cerr << std::format("[{}] Failed to evaluate expression: {}\n", id(), mask_result.error().message_);
      return std::unexpected(mask_result.error());
    }

    auto& mask_col = **mask_result;
    std::vector<size_t> indices;
    indices.reserve(batch->num_rows());

    for (size_t i = 0; i < batch->num_rows(); ++i) {
      auto val = mask_col.GetValue(i);
      if (auto* bool_val = std::get_if<bool>(&val)) {
        if (*bool_val) {
          indices.push_back(i);
        }
      }
    }

    if (indices.empty()) {
      auto output_batch = RecordBatch(batch->schema(), {});
      return std::make_shared<HostMemoryBlock>("output", std::move(output_batch));
    }

    std::vector<std::shared_ptr<datasource::ColumnVector>> new_columns;
    new_columns.reserve(batch->num_columns());

    for (size_t col_idx = 0; col_idx < batch->num_columns(); ++col_idx) {
      const auto& col = batch->column(col_idx);
      auto filtered = MyFilterColumn(col, indices);
      new_columns.push_back(std::move(filtered));
    }
    auto output_batch = RecordBatch(batch->schema(), std::move(new_columns));
    return std::make_shared<HostMemoryBlock>("output", std::move(output_batch));
  }

 private:
  Selection* selection;
};

/// @brief Project operator that selects columns
class ProjectOperator : public SimpleOperator {
 public:
  ProjectOperator(std::uint32_t id, Projection* projection)
      : SimpleOperator(id, std::format("Project({})", projection->ToString()), OperatorType::Project),
        projection(projection) {}

 protected:
  [[nodiscard]] Result<std::shared_ptr<DataBlock>> ProcessBatch(
      std::shared_ptr<DataBlock> input) override {
    std::cout << std::format("[{}] Projecting columns: {}\n", id(), projection->ToString());

    std::vector<ExpressionPtr> exprs;
    exprs.reserve(projection->expressions().size());
    for (const auto& expr : projection->expressions()) {
      auto expr_result = MyCreatePhysicalExpr(*expr, projection->input().schema());
      if (!expr_result) {
        return std::unexpected(expr_result.error());
      }
      exprs.push_back(std::move(*expr_result));
    }

    auto* cpu_result = static_cast<HostMemoryBlock*>(input.get());;
    auto *batch = cpu_result->record_batch();


    std::vector<std::shared_ptr<datasource::ColumnVector>> columns;
    columns.reserve(exprs.size());
    for (const auto& expr : exprs) {
      auto result = expr->Evaluate(*batch);
      if (result.has_value()) {
        columns.push_back(std::move(result.value()));
      }
    }
    std::vector<Field> fields;
    fields.reserve(projection->expressions().size());
    for (const auto& expr : projection->expressions()) {
      auto field_result = expr->ToField(projection->input());
      if (!field_result) {
        return std::unexpected(field_result.error());
      }
      fields.push_back(*field_result);
    }
    auto output_batch = RecordBatch(Schema(std::move(fields)), std::move(columns));
    return std::make_shared<HostMemoryBlock>("output", std::move(output_batch));
  }

 private:
  Projection* projection;
};

/// @brief Output operator that consumes results
class OutputOperator : public Operator {
 public:
  explicit OutputOperator(std::uint32_t id) : Operator(id, "Output", OperatorType::Output) {}

  [[nodiscard]] bool can_you_throttle_my_input() const override { return false; }

  [[nodiscard]] ExecutionStatus run() override {
    std::cout << std::format("[{}] Output operator starting\n", id());

    std::size_t total_rows = 0;
    std::size_t total_bytes = 0;
    std::size_t batch_count = 0;

    auto input_holder = InputCache();
    if (!input_holder) {
      std::cerr << std::format("[{}] No input cache\n", id());
      return ExecutionStatus::Stop;
    }

    while (true) {
      auto result = input_holder->Pull();

      
      if (!result.has_value()) {
        break;
      }

      auto* cpu_result = static_cast<HostMemoryBlock*>(result.value().get());;
      auto *batch = cpu_result->record_batch();
      std::cout << batch->ToString() << "\n";

      auto& msg = result.value();
      if (msg) {
        total_rows += msg->num_rows();
        total_bytes += msg->size_in_bytes();
        ++batch_count;
      }
    }

    std::cout << std::format("[{}] Output complete: {} batches, {} rows, {} bytes\n", id(),
                             batch_count, total_rows, total_bytes);

    return ExecutionStatus::Proceed;
  }
};

// =============================================================================
// Main
// =============================================================================

// std::shared_ptr<Graph> CreateExecutionGraph(const LogicalPlan& plan) {
//
//   return null;
// }

int main() {
  std::cout << "=== Simple Graph Example ===\n\n";

  std::string filename = "/Users/alexander/git/bigger-than-gpu/testdata/employees.parquet";
  auto data_source = datasource::ParquetDataSource::Open(filename);
  if (!data_source) {
    std::cerr << "Failed to open Parquet file: " << data_source.error() << "\n";
    return 1;
  }

  auto source = std::make_shared<datasource::ParquetDataSource>(std::move(data_source.value()));
  //
  // // Build: SELECT name, salary FROM employees WHERE salary < 100
  auto scan = std::make_unique<Scan>("employees", source, std::nullopt);
  Scan* scan_ptr = scan.get();

  // Create filter: salary < 100
  auto filter_expr = lt(col("department_id"), lit(5));
  auto selection = std::make_unique<Selection>(std::move(scan), std::move(filter_expr));
  Selection* selection_ptr = selection.get();

  // Create projection: name, salary
  std::vector<LogicalExprPtr> projection_cols;
  projection_cols.push_back(col("name"));
  projection_cols.push_back(col("department_id"));

  auto projection =
      std::make_unique<Projection>(std::move(selection), std::move(projection_cols));
  Projection* projection_ptr = projection.get();

  std::cout << "Logical Plan:\n" << projection->Pretty() << "\n";

  // Create memory manager
  auto memory_manager = quest::make_default_memory_manager();

  // Create graph
  auto graph = std::make_shared<Graph>(memory_manager);

  // Create operators
  TableScanOperator scan_node(0, scan_ptr);
  FilterOperator filter_node(1, selection_ptr);
  ProjectOperator project_node(2, projection_ptr);
  OutputOperator output_node(3);

  // Build execution graph using DSL
  // scan -> filter -> project -> output

  *graph += scan_node >> filter_node;
  *graph += filter_node >> project_node;
  *graph += project_node >> output_node;

  // Show graph structure
  graph->PrintStructure();

  std::cout << "\n--- Executing Graph ---\n\n";

  // Execute
  auto status = graph->ExecuteLegacy();

  if (status.ok()) {
    std::cout << "\n=== Execution completed successfully ===\n";
  } else {
    std::cerr << "\n=== Execution failed: " << status.message() << " ===\n";
    return 1;
  }
  return 0;
}