/// @file simple_graph.cpp
/// @brief Example demonstrating the taskflow DSL for a simple query pipeline

#include <format>
#include <iostream>
#include <quest/quest.hpp>
#include <quest/taskflow/graph.hpp>

#include "quest/datasource/data_source.hpp"

using namespace quest;
using namespace quest::taskflow;
using namespace quest::cache;

// =============================================================================
// Example Operators
// =============================================================================

/// @brief Table scan operator that generates sample data
class TableScanOperator : public Operator {
public:
    TableScanOperator(std::uint32_t id, std::string table_name, std::shared_ptr<datasource::DataSource> data_source)
        : Operator(id, std::format("TableScan({})", table_name), OperatorType::TableScan)
        , table_name_(std::move(table_name))
        , data_source_(data_source) {}
    
    [[nodiscard]] bool can_you_throttle_my_input() const override { return false; }
    
    [[nodiscard]] ExecutionStatus run() override {
        std::cout << std::format("[{}] Starting scan of '{}'\n", id(), table_name_);
      std::vector<std::string> proj;
        for (const auto& field : data_source_->schema().fields()) {
          proj.push_back(field.name);
        }


        auto sequence = data_source_->Scan(proj);

      for (auto iter = sequence.begin(); iter != sequence.end(); ++iter) {
        auto batch_result = *iter;
        if (!batch_result) {
          return ExecutionStatus::Stop;
        }
        // callback(std::move(*batch_result));
        auto cache_data = std::move(batch_result.value());
        auto add_status = add_to_output(std::move(cache_data));
        if (!add_status.ok()) {
          // Failed to add to output - stop execution.
          return ExecutionStatus::Stop;
        }
      }

        std::cout << std::format("[{}] Scan complete\n", id());
        return ExecutionStatus::Proceed;
    }

private:
    std::string table_name_;
    std::shared_ptr<datasource::DataSource>  data_source_;
};

/// @brief Filter operator that passes through data
class FilterOperator : public SimpleOperator {
public:
    FilterOperator(std::uint32_t id, std::string condition)
        : SimpleOperator(id, std::format("Filter({})", condition), OperatorType::Filter)
        , condition_(std::move(condition)) {}

protected:
    [[nodiscard]] Result<std::shared_ptr<CacheData>> process_batch(
        std::shared_ptr<CacheData> input) override {
        
        std::cout << std::format("[{}] Filtering batch with condition: {}\n", 
            id(), condition_);
        
        // In a real implementation, apply the filter condition
        // For now, just pass through
        return input;
    }

private:
    std::string condition_;
};

/// @brief Project operator that selects columns
class ProjectOperator : public SimpleOperator {
public:
    ProjectOperator(std::uint32_t id, std::string columns)
        : SimpleOperator(id, std::format("Project({})", columns), OperatorType::Project)
        , columns_(std::move(columns)) {}

protected:
    [[nodiscard]] Result<std::shared_ptr<CacheData>> process_batch(
        std::shared_ptr<CacheData> input) override {
        
        std::cout << std::format("[{}] Projecting columns: {}\n", id(), columns_);
        
        // In a real implementation, select the specified columns
        return input;
    }

private:
    std::string columns_;
};

/// @brief Output operator that consumes results
class OutputOperator : public Operator {
public:
    explicit OutputOperator(std::uint32_t id)
        : Operator(id, "Output", OperatorType::Output) {}
    
    [[nodiscard]] bool can_you_throttle_my_input() const override { return false; }
    
    [[nodiscard]] ExecutionStatus run() override {
        std::cout << std::format("[{}] Output operator starting\n", id());
        
        std::size_t total_rows = 0;
        std::size_t total_bytes = 0;
        std::size_t batch_count = 0;
        
        auto input_holder = input_cache();
        if (!input_holder) {
            std::cerr << std::format("[{}] No input cache\n", id());
            return ExecutionStatus::Stop;
        }
        
        while (true) {
            auto result = input_holder->pull();
            if (!result.has_value()) {
                break;
            }
            
            auto& msg = result.value();
            if (msg && msg->data()) {
                total_rows += msg->data()->num_rows();
                total_bytes += msg->data()->size_in_bytes();
                ++batch_count;
            }
        }
        
        std::cout << std::format(
            "[{}] Output complete: {} batches, {} rows, {} bytes\n",
            id(), batch_count, total_rows, total_bytes);
        
        return ExecutionStatus::Proceed;
    }
};

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== Simple Graph Example ===\n\n";
    
    // Create memory manager
    auto memory_manager = quest::make_default_memory_manager();
    
    // Create graph
    auto graph = std::make_shared<Graph>(memory_manager);
    
    // Create operators
    TableScanOperator scan(0, "customers", 3);
    FilterOperator filter(1, "age > 21");
    ProjectOperator project(2, "name, email");
    OutputOperator output(3);
    
    // Build execution graph using DSL
    // scan -> filter -> project -> output
    *graph += scan >> filter;
    *graph += filter >> project;
    *graph += project >> output;
    
    // Show graph structure
    graph->PrintStructure();

    std::cout << "\n--- Executing Graph ---\n\n";

    // Execute
    auto status = graph->Execute();
    
    if (status.ok()) {
        std::cout << "\n=== Execution completed successfully ===\n";
    } else {
        std::cerr << "\n=== Execution failed: " << status.message() << " ===\n";
        return 1;
    }
    
    return 0;
}
