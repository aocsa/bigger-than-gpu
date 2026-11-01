# QUEST — Bigger Than RAM

A modern C++23 batch processing framework with two-tier memory (CPU/Disk) and declarative dataflow execution.

Inspired by BlazingSQL, refocused for CPU → Disk tiers — handling datasets larger than available RAM by intelligently spilling to disk.

## Quick Start

```bash
# Dependencies (macOS)
brew install cmake googletest snappy

# Build
mkdir -p build && cd build && cmake .. && make

# Test
ctest --output-on-failure
```

Requires C++23 (GCC 13+ or Clang 17+) and CMake 3.25+.

## Usage

### Query Builder

```cpp
#include "quest/builder/query_builder.hpp"
using namespace quest;

auto batches = std::move(QueryBuilder::FromParquet("data.parquet").value())
    .Where(Col("age") > Lit(21))
    .Select({"id", "name"})
    .ExecuteOnGraph();
```

### Dataflow DSL

```cpp
// Simple chain
graph += scan >> filter >> project >> output;

// Named ports for multi-input operators
graph += customers >> join["input_a"];
graph += orders >> join["input_b"];

// With edge configuration
graph += link(sort, merge, EdgeConfig::Concatenating());
```

## Architecture

```
QueryBuilder → LogicalPlan → TaskFlow Graph → Cache → Memory
```

Three subsystems:
- **TaskFlow** — execution graph, operator scheduling, BFS traversal
- **Cache** — BatchHolders (FIFO + Concatenating) with thread-safe BlockingQueue
- **Memory** — two-tier CPU/Disk with auto-spill at 85% saturation

For detailed architecture, class diagrams, and design patterns: [`docs/dev/ARCHITECTURE.md`](docs/dev/ARCHITECTURE.md)

## Project Structure

```
src/quest/
├── quest.hpp       # Umbrella header for users (not included inside src/)
├── builder/        # QueryBuilder, ExprBuilder
├── common/         # Result / Status / Error
├── columnar/       # Types, schema, column vectors, record batches, serializer
├── datasource/     # DataSource interface (partition-indexed scan)
├── io/parquet/     # Parquet reader and ParquetDataSource
├── plan/
│   ├── logical/    # Logical plan nodes and expressions
│   └── physical/   # Physical expressions evaluated by operators
├── execution/
│   ├── kernels/    # Batch filter/project/truncate kernels, bloom filter
│   └── lowering/   # plan_to_graph: LogicalPlan → taskflow Graph
├── taskflow/       # Graph, Operator, Port, pipelines, operators/
├── cache/          # BatchHolder, DataBlock, BlockingQueue
└── memory/         # CPU/Disk memory resources
```

## Claude Code Commands

This project includes slash commands for [Claude Code](https://claude.ai/code) that automate common development workflows.

**Scaffolding**

| Command | What it does | Example |
|---------|-------------|---------|
| `/new-op` | Scaffold a new operator (enum, class, test, CMake) | `/new-op Sort` |
| `/new-node` | Add a logical + physical plan node with QueryPlan wiring | `/new-node Limit` |

**Review & Analysis**

| Command | What it does | Example |
|---------|-------------|---------|
| `/verify` | Adversarial 6-point stress test on an operator | `/verify FilterOperator` |
| `/trace` | Read-only trace of a query through all 6 layers | `/trace Where(Col("age") > Lit(21))` |
| `/review-mem` | Review code against the ownership model | `/review-mem src/quest/cache/data_block.hpp` |
| `/diagnose` | Run clang-tidy on changed files | `/diagnose src/quest/taskflow/graph.cpp` |

**Build & Run**

| Command | What it does | Example |
|---------|-------------|---------|
| `/build-test` | Build and run all tests | `/build-test` |
| `/test` | Run a specific test by name or pattern | `/test graph` or `/test "*Join*"` |
| `/run` | Build and run an example | `/run parquet_reader_demo` |
| `/format` | Run clang-format on changed files | `/format` or `/format src/quest/cache/*.hpp` |
| `/commit` | Create a conventional commit from staged changes | `/commit` or `/commit "fix: race in scheduler"` |

Commands require the `/project:` prefix since they are project-scoped. Without arguments, they'll ask what you need.

### Examples

```bash
# Scaffold a new operator (asks stateless/stateful, port count)
/project:new-op Limit

# Run a specific test
/project:test graph

# Run with GTest filter
/project:test "*Join*"

# Build and run an example
/project:run parquet_reader_demo

# Format all changed C++ files
/project:format

# Create a commit (analyzes staged changes, drafts message)
/project:commit

# Create a commit with a specific message
/project:commit "refactor(cache): simplify BatchHolder spill logic"

# Review memory safety on uncommitted changes
/project:review-mem

# Trace a query through all 6 QUEST layers
/project:trace Where(Col("age") > Lit(21))
```

## Contributing

See [`docs/dev/CONTRIBUTING.md`](docs/dev/CONTRIBUTING.md) for build setup, test organization, and debugging guide.

## License

MIT
