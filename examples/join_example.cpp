/// @file join_example.cpp
/// @brief Example demonstrating a join query with named ports
/// @details Implements the DSL pattern:
///   graph += table_scan_a >> join["input_a"];
///   graph += table_scan_b >> join["input_b"];
///   graph += join >> output;

#include <quest/quest.hpp>
#include <iostream>
#include <format>

#include "quest/datasource/column_vector.hpp"
#include "quest/datasource/record_batch.hpp"
#include "quest/datasource/schema.hpp"
#include "quest/datasource/types.hpp"

using namespace quest;
using namespace quest::taskflow;
using namespace quest::cache;
using namespace quest::datasource;

// =============================================================================
// Helper Functions
// =============================================================================

/// @brief Creates a simple RecordBatch for testing
RecordBatch MakeTestBatch(std::size_t num_rows, std::size_t num_columns) {
    std::vector<Field> fields;
    std::vector<std::shared_ptr<ColumnVector>> columns;

    for (std::size_t c = 0; c < num_columns; ++c) {
        fields.emplace_back("col" + std::to_string(c), Int32Type{});
        auto col = std::make_shared<TypedColumnVector<int32_t>>(Int32Type{}, num_rows);
        for (std::size_t r = 0; r < num_rows; ++r) {
            col->Append(static_cast<int32_t>(r * num_columns + c));
        }
        columns.push_back(col);
    }

    Schema schema(fields);
    return RecordBatch(schema, columns);
}

// =============================================================================
// Example Operators
// =============================================================================

/// @brief Table scan operator
class TableScanOperator : public Operator {
public:
    TableScanOperator(std::uint32_t id, std::string table_name, std::size_t num_batches = 3)
        : Operator(id, std::format("TableScan({})", table_name), OperatorType::TableScan)
        , table_name_(std::move(table_name))
        , num_batches_(num_batches) {}

    [[nodiscard]] bool can_you_throttle_my_input() const override { return false; }

    [[nodiscard]] ExecutionStatus run() override {
        std::cout << std::format("[{}] Scanning table '{}'\n", id(), table_name_);

        for (std::size_t i = 0; i < num_batches_; ++i) {
            auto batch = MakeTestBatch(50, 3);
            auto data = std::make_shared<HostMemoryBlock>(
                std::format("{}_{}", table_name_, i),
                std::move(batch)
            );

            auto status = AddToOutput(std::move(data));
            if (!status.ok()) {
                return ExecutionStatus::Stop;
            }

            std::cout << std::format("[{}] {} batch {}\n", id(), table_name_, i + 1);
        }

        return ExecutionStatus::Proceed;
    }

private:
    std::string table_name_;
    std::size_t num_batches_;
};

/// @brief Pairwise join operator with two named inputs
class PartwiseJoinOperator : public Operator {
public:
    PartwiseJoinOperator(std::uint32_t id, std::string condition)
        : Operator(id, std::format("PartwiseJoin({})", condition), OperatorType::PartwiseJoin)
        , condition_(std::move(condition)) {
        // Register named input ports
        input().RegisterPort("input_a");
        input().RegisterPort("input_b");
    }
    
    [[nodiscard]] bool can_you_throttle_my_input() const override { return true; }
    
    [[nodiscard]] ExecutionStatus run() override {
        std::cout << std::format("[{}] Join starting with condition: {}\n", id(), condition_);
        
        auto& cache_a = input()["input_a"];
        auto& cache_b = input()["input_b"];
        
        if (!cache_a || !cache_b) {
            std::cerr << std::format("[{}] Missing input caches\n", id());
            return ExecutionStatus::Stop;
        }
        
        // Collect all batches from both sides
        std::vector<std::shared_ptr<DataBlock>> batches_a;
        std::vector<std::shared_ptr<DataBlock>> batches_b;
        
        // Read from input_a
        while (true) {
            auto result = cache_a->Pull();
            if (!result.has_value()) break;
            if (result.value() && result.value()) {
                batches_a.push_back(result.value());
            }
        }
        
        // Read from input_b
        while (true) {
            auto result = cache_b->Pull();
            if (!result.has_value()) break;
            if (result.value() && result.value()) {
                batches_b.push_back(result.value());
            }
        }
        
        std::cout << std::format("[{}] Got {} batches from A, {} batches from B\n",
            id(), batches_a.size(), batches_b.size());
        
        // Perform cartesian join (simplified)
        std::size_t join_count = 0;
        for (const auto& a : batches_a) {
            for (const auto& b : batches_b) {
                // Create joined batch with combined columns
                auto joined_rows = a->num_rows() * b->num_rows();
                auto joined_cols = a->num_columns() + b->num_columns();
                auto batch = MakeTestBatch(joined_rows, joined_cols);
                auto joined = std::make_shared<HostMemoryBlock>(
                    std::format("joined_{}_{}", id(), join_count),
                    std::move(batch)
                );

                auto status = AddToOutput(std::move(joined));
                if (!status.ok()) {
                    return ExecutionStatus::Stop;
                }

                ++join_count;
            }
        }
        
        std::cout << std::format("[{}] Produced {} joined batches\n", id(), join_count);
        
        return ExecutionStatus::Proceed;
    }

private:
    std::string condition_;
};

/// @brief Output operator
class OutputOperator : public Operator {
public:
    explicit OutputOperator(std::uint32_t id)
        : Operator(id, "Output", OperatorType::Output) {}
    
    [[nodiscard]] bool can_you_throttle_my_input() const override { return false; }
    
    [[nodiscard]] ExecutionStatus run() override {
        std::size_t total_rows = 0;
        std::size_t batch_count = 0;
        
        auto input_holder = InputCache();
        while (input_holder) {
            auto result = input_holder->Pull();
            if (!result.has_value()) break;
            
            if (result.value() && result.value()) {
                total_rows += result.value()->num_rows();
                ++batch_count;
            }
        }
        
        std::cout << std::format("[{}] Final output: {} batches, {} total rows\n",
            id(), batch_count, total_rows);
        
        return ExecutionStatus::Proceed;
    }
};

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== Join Example with Named Ports ===\n\n";
    
    auto memory_manager = make_default_memory_manager();
    auto graph = std::make_shared<Graph>(memory_manager);
    
    // Create operators
    TableScanOperator scan_a(0, "customers", 2);
    TableScanOperator scan_b(1, "orders", 3);
    PartwiseJoinOperator join(2, "customers.id = orders.customer_id");
    OutputOperator output(3);
    
    // Build graph using the DSL with named ports
    // This is the key pattern from BlazingSQL:
    //   graph += table_scan_a >> join["input_a"];
    //   graph += table_scan_b >> join["input_b"];
    
    *graph += scan_a >> join["input_a"];
    *graph += scan_b >> join["input_b"];
    *graph += join >> output;
    
    // Show graph
    graph->PrintStructure();

    std::cout << "\n--- Executing Join ---\n\n";

    auto status = graph->ExecuteLegacy();
    
    if (status.ok()) {
        std::cout << "\n=== Join completed successfully ===\n";
    } else {
        std::cerr << "\n=== Join failed: " << status.message() << " ===\n";
        return 1;
    }
    
    return 0;
}
