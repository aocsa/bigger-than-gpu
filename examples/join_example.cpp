/// @file join_example.cpp
/// @brief Example demonstrating a join query with named ports
/// @details Implements the DSL pattern:
///   graph += table_scan_a >> join["input_a"];
///   graph += table_scan_b >> join["input_b"];
///   graph += join >> output;

#include <quest/quest.hpp>
#include <iostream>
#include <format>

using namespace quest;
using namespace quest::taskflow;
using namespace quest::cache;

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
            auto data = std::make_shared<CPUCacheData>(
                std::format("{}_{}", table_name_, i),
                std::vector<std::byte>(512),
                50,
                3
            );
            
            auto status = add_to_output(std::move(data));
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
        input().register_port("input_a");
        input().register_port("input_b");
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
        std::vector<std::shared_ptr<CacheData>> batches_a;
        std::vector<std::shared_ptr<CacheData>> batches_b;
        
        // Read from input_a
        while (true) {
            auto result = cache_a->pull();
            if (!result.has_value()) break;
            if (result.value() && result.value()->data()) {
                batches_a.push_back(result.value()->data());
            }
        }
        
        // Read from input_b
        while (true) {
            auto result = cache_b->pull();
            if (!result.has_value()) break;
            if (result.value() && result.value()->data()) {
                batches_b.push_back(result.value()->data());
            }
        }
        
        std::cout << std::format("[{}] Got {} batches from A, {} batches from B\n",
            id(), batches_a.size(), batches_b.size());
        
        // Perform cartesian join (simplified)
        std::size_t join_count = 0;
        for (const auto& a : batches_a) {
            for (const auto& b : batches_b) {
                // Create joined batch
                auto joined = std::make_shared<CPUCacheData>(
                    std::format("joined_{}_{}", id(), join_count),
                    std::vector<std::byte>(1024),
                    a->num_rows() * b->num_rows(),  // Cartesian product
                    a->num_columns() + b->num_columns()
                );
                
                auto status = add_to_output(std::move(joined));
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
        
        auto input_holder = input_cache();
        while (input_holder) {
            auto result = input_holder->pull();
            if (!result.has_value()) break;
            
            if (result.value() && result.value()->data()) {
                total_rows += result.value()->data()->num_rows();
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

    auto status = graph->Execute();
    
    if (status.ok()) {
        std::cout << "\n=== Join completed successfully ===\n";
    } else {
        std::cerr << "\n=== Join failed: " << status.message() << " ===\n";
        return 1;
    }
    
    return 0;
}
