# Bigger Than GPU (QUEST)

A modern C++23 batch processing framework with a two-tier memory hierarchy (CPU/Disk) and a declarative dataflow execution model.

## Overview

QUEST is a reimplementation of the BlazingSQL "Bigger than GPU" architecture, refocused for CPU and Disk memory tiers. It provides a framework for building data processing pipelines that can handle datasets larger than available RAM by intelligently spilling to disk when memory pressure is high.

### Key Features

- **Declarative Dataflow DSL** - Build execution graphs using intuitive operator chaining
- **Two-Tier Memory Hierarchy** - Automatic spilling from CPU RAM to Disk when memory is saturated
- **Parallel Execution** - Operators execute concurrently with dependency-aware scheduling
- **Flow Control** - Backpressure mechanisms prevent memory exhaustion
- **Modern C++23** - Uses `std::expected`, concepts, ranges, and `std::format`

## Quick Start

### Building

```bash
mkdir build && cd build
cmake ..
make
```

Requirements:
- C++23 compatible compiler (GCC 13+, Clang 17+)
- CMake 3.25+

### Hello World Example

```cpp
#include <quest/quest.hpp>

using namespace quest::taskflow;

int main() {
    // Create execution graph
    auto graph = std::make_shared<Graph>();
    
    // Define operators
    TableScanOperator scan(0, "customers");
    FilterOperator filter(1, "age > 21");
    ProjectOperator project(2, "name, email");
    OutputOperator output(3);
    
    // Build pipeline using DSL
    *graph += scan >> filter;
    *graph += filter >> project;
    *graph += project >> output;
    
    // Execute
    graph->execute();
    
    return 0;
}
```

### Join Example with Named Ports

```cpp
#include <quest/quest.hpp>

using namespace quest::taskflow;

int main() {
    auto graph = std::make_shared<Graph>();
    
    TableScanOperator customers(0, "customers");
    TableScanOperator orders(1, "orders");
    JoinOperator join(2, "customers.id = orders.customer_id");
    OutputOperator output(3);
    
    // Multiple inputs using named ports
    *graph += customers >> join["input_a"];
    *graph += orders >> join["input_b"];
    *graph += join >> output;
    
    graph->execute();
    
    return 0;
}
```

## The DSL

QUEST provides a fluent interface for building execution graphs:

```cpp
// Simple chaining
graph += a >> b >> c;

// Named ports for multi-input operators
graph += source_a >> join["input_a"];
graph += source_b >> join["input_b"];

// With edge configuration
graph += link(a, b, EdgeConfig::Concatenating());
graph += link(partition, merge, EdgeConfig::Partitioned(32));

// Flow control configuration
graph += link(a, b, EdgeConfig::Simple()
    .with_flow_control(/*max_batches=*/10, /*max_bytes=*/1024*1024));
```

## Architecture

QUEST is composed of three main subsystems:

1. **TaskFlow** - Execution graph and operator scheduling
2. **Cache System** - Data transfer between operators via BatchHolders
3. **Memory Resources** - Two-tier memory management (CPU/Disk)

```
┌─────────────────────────────────────────────────────────┐
│                    Execution Graph                       │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐          │
│  │ TableScan│───▶│  Filter  │───▶│  Output  │          │
│  └──────────┘    └──────────┘    └──────────┘          │
│        │              │               │                 │
│        ▼              ▼               ▼                 │
│   BatchHolder    BatchHolder    BatchHolder             │
│        │              │               │                 │
└────────┼──────────────┼───────────────┼─────────────────┘
         │              │               │
         ▼              ▼               ▼
┌─────────────────────────────────────────────────────────┐
│              Memory Resource Manager                     │
│  ┌─────────────────┐      ┌─────────────────┐          │
│  │ CPU Memory (L1) │ ───▶ │ Disk Cache (L2) │          │
│  │   (Primary)     │ spill│   (Secondary)   │          │
│  └─────────────────┘      └─────────────────┘          │
└─────────────────────────────────────────────────────────┘
```

For detailed architecture documentation, see [ARCHITECTURE.md](ARCHITECTURE.md).

## Project Structure

```
bigger-than-gpu/
├── include/quest/
│   ├── quest.hpp                 # Main include header
│   ├── result.hpp              # Result/Status types
│   ├── taskflow/
│   │   ├── graph.hpp           # Execution graph
│   │   ├── operator.hpp        # Base operator class
│   │   ├── operator_type.hpp   # Operator type enum
│   │   ├── kpair.hpp           # DSL (>> operator)
│   │   └── port.hpp            # Input/output ports
│   ├── cache/
│   │   ├── batch_holder.hpp    # Data transfer between operators
│   │   ├── data_block.hpp      # CPU/Disk data representations
│   │   └── blocking_queue.hpp  # Thread-safe queue
│   └── memory/
│       └── memory_resource.hpp # CPU/Disk memory management
├── src/                        # Implementation files
├── examples/                   # Example programs
└── tests/                      # Unit tests
```

## Running Tests

```bash
cd build
ctest --output-on-failure
```

Or run individually:

```bash
./tests/test_blocking_queue
./tests/test_graph
```

## Running Examples

```bash
./examples/simple_graph
./examples/join_example
```

## Origin and Inspiration

This project is inspired by the BlazingSQL "Bigger than GPU" architecture, which enabled GPU-accelerated SQL queries on datasets larger than GPU memory. The original system used a three-tier hierarchy (GPU → CPU → Disk), while QUEST focuses on the CPU → Disk tiers for broader applicability.

Key references:
- [BlazingSQL Interview on Bigger than GPU](https://www.youtube.com/watch?v=vhXy-LJCsOQ)
- Original BlazingSQL codebase architecture

## License

MIT License

## Contributing

Contributions are welcome! Please feel free to submit issues and pull requests.