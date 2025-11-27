/// @file simple_graph.cpp
/// @brief Example demonstrating the taskflow DSL for a simple query pipeline

#include <quest/quest.hpp>
#include <quest/taskflow/graph.hpp>
#include <iostream>
#include <format>

using namespace quest;
using namespace quest::taskflow;
using namespace quest::cache;

// =============================================================================
// Example Operators
// =============================================================================

/// @brief Table scan operator that generates sample data
class TableScanOperator : public Operator {
public:
    TableScanOperator(std::uint32_t id, std::string table_name, std::size_t num_batches = 5)
        : Operator(id, std::format("TableScan({})", table_name), OperatorType::TableScan)
        , table_name_(std::move(table_name))
        , num_batches_(num_batches) {}
    
    [[nodiscard]] bool can_you_throttle_my_input() const override { return false; }
    
    [[nodiscard]] ExecutionStatus run() override {
        std::cout << std::format("[{}] Starting scan of '{}'\n", id(), table_name_);
        
        for (std::size_t i = 0; i < num_batches_; ++i) {
            // Generate sample batch (1KB of data, 100 rows, 4 columns)
            auto data = std::make_shared<CPUCacheData>(
                std::format("batch_{}_{}", id(), i),
                std::vector<std::byte>(1024),
                100,  // rows
                4     // columns
            );
            
            auto status = add_to_output(std::move(data));
            if (!status.ok()) {
                std::cerr << std::format("[{}] Failed to add batch: {}\n", 
                    id(), status.message());
                return ExecutionStatus::Stop;
            }
            
            std::cout << std::format("[{}] Produced batch {}/{}\n", 
                id(), i + 1, num_batches_);
            
            // Flow control
            wait_if_output_saturated();
        }
        
        std::cout << std::format("[{}] Scan complete\n", id());
        return ExecutionStatus::Proceed;
    }

private:
    std::string table_name_;
    std::size_t num_batches_;
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
