# QUEST Architecture

This document describes the architecture and design of the Bigger Than GPU (QUEST) framework.

## Table of Contents

1. [System Overview](#system-overview)
2. [Core Concepts](#core-concepts)
3. [TaskFlow System](#taskflow-system)
4. [Cache System](#cache-system)
5. [Memory Management](#memory-management)
6. [Execution Model](#execution-model)
7. [Data Flow](#data-flow)
8. [Class Diagrams](#class-diagrams)
9. [Design Patterns](#design-patterns)
10. [Extension Points](#extension-points)

---

## System Overview

QUEST is a dataflow execution framework designed for batch processing of large datasets. The architecture follows a producer-consumer model where **Operators** produce and consume data **Batches** through **BatchHolders**, with automatic memory management across CPU and Disk tiers.

### High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         USER APPLICATION                             │
│                                                                      │
│    graph += scan >> filter >> project >> output;                    │
│                                                                      │
└──────────────────────────────┬──────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         TASKFLOW LAYER                               │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    Execution Graph                           │   │
│  │  ┌────────┐   ┌────────┐   ┌────────┐   ┌────────┐         │   │
│  │  │Operator│──▶│Operator│──▶│Operator│──▶│Operator│         │   │
│  │  │ (Scan) │   │(Filter)│   │(Project)   │(Output)│         │   │
│  │  └────────┘   └────────┘   └────────┘   └────────┘         │   │
│  └─────────────────────────────────────────────────────────────┘   │
└──────────────────────────────┬──────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         CACHE LAYER                                  │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐                 │
│  │ BatchHolder │  │ BatchHolder │  │ BatchHolder │                 │
│  │             │  │             │  │             │                 │
│  │ [Queue of   │  │ [Queue of   │  │ [Queue of   │                 │
│  │  CacheData] │  │  CacheData] │  │  CacheData] │                 │
│  └─────────────┘  └─────────────┘  └─────────────┘                 │
└──────────────────────────────┬──────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      MEMORY MANAGEMENT LAYER                         │
│  ┌──────────────────────────┐  ┌──────────────────────────┐        │
│  │   CPUMemoryResource      │  │   DiskMemoryResource     │        │
│  │   (L1 - Primary)         │  │   (L2 - Secondary)       │        │
│  │                          │  │                          │        │
│  │   - Track allocations    │  │   - File-based storage   │        │
│  │   - Saturation detection │  │   - Cleanup on destroy   │        │
│  └──────────────────────────┘  └──────────────────────────┘        │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Core Concepts

### Operator (formerly Kernel)

An **Operator** is a computation unit in the execution graph. Each operator:
- Has input and output **Ports**
- Contains an **expression** describing its operation
- Implements a `run()` method that performs the computation
- Can be throttled for flow control

```cpp
class Operator {
    Port input_;      // Input connections
    Port output_;     // Output connections
    std::string expression_;
    OperatorType type_;
    
    virtual ExecutionStatus run() = 0;
};
```

### Port

A **Port** manages connections between operators. Each port can have multiple named **BatchHolders** for operators that need multiple inputs (like joins).

```cpp
class Port {
    std::map<std::string, std::shared_ptr<IBatchHolder>> batch_holders_;
};
```

### BatchHolder (formerly CacheMachine)

A **BatchHolder** is the conduit for data transfer between operators. It:
- Uses a thread-safe **BlockingQueue** internally
- Supports flow control (backpressure)
- Can automatically spill to disk when memory is saturated

```cpp
class IBatchHolder {
    virtual Status add(CacheMessagePtr message) = 0;
    virtual Result<CacheMessagePtr> pull() = 0;
    virtual void finish() = 0;
};
```

### CacheData

**CacheData** represents data that can be stored in different memory tiers:
- **CPUCacheData** - Data in RAM (vector of bytes)
- **DiskCacheData** - Data on disk (file reference)

Both can convert between tiers via `to_cpu()` and `to_disk()` methods.

---

## TaskFlow System

The TaskFlow system manages the execution graph and operator scheduling.

### Graph Structure

```
         ┌─────────────────────────────────────────┐
         │              Sentinel Node              │
         │                (id = -1)                │
         └──────────────────┬──────────────────────┘
                            │
              ┌─────────────┼─────────────┐
              │             │             │
              ▼             ▼             ▼
         ┌────────┐    ┌────────┐    ┌────────┐
         │ Source │    │ Source │    │ Source │
         │   A    │    │   B    │    │   C    │
         └───┬────┘    └───┬────┘    └───┬────┘
             │             │             │
             │             ▼             │
             │        ┌────────┐         │
             └───────▶│  Join  │◀────────┘
                      └───┬────┘
                          │
                          ▼
                     ┌────────┐
                     │ Output │
                     └────────┘
```

### Edge Representation

Edges connect operators and carry configuration:

```cpp
struct Edge {
    int32_t source;           // Source operator ID
    int32_t target;           // Target operator ID  
    std::string source_port;  // Output port name
    std::string target_port;  // Input port name
};

struct EdgeConfig {
    BatchHolderType type;     // Simple or Concatenating
    size_t num_partitions;    // For partitioned outputs
    size_t flow_control_batches_threshold;
    size_t flow_control_bytes_threshold;
};
```

### The DSL

The DSL provides a fluent interface for building graphs:

```cpp
// Operator >> Operator (default ports)
KPair operator>>(Operator& src, Operator& dst);

// Operator >> Operator["port"] (named destination port)
KPair operator>>(Operator& src, OperatorPortPair dst);

// Operator["port"] >> Operator (named source port)
KPair operator>>(OperatorPortPair src, Operator& dst);

// link() function for configuration
KPair link(Operator& src, Operator& dst, EdgeConfig config);
```

Usage:

```cpp
// Simple chain
graph += scan >> filter >> project;

// Named ports
graph += table_a >> join["input_a"];
graph += table_b >> join["input_b"];

// With configuration
graph += link(sort, merge, EdgeConfig::Concatenating());
```

---

## Cache System

The cache system handles data transfer between operators.

### BatchHolder Types

| Type | Behavior | Use Case |
|------|----------|----------|
| **Simple** | Each batch pulled individually | Most operators |
| **Concatenating** | All batches merged into one | Aggregations, final output |

### Message Flow

```
Producer Operator                              Consumer Operator
      │                                              │
      │  ┌──────────────────────────────────────┐   │
      │  │           BatchHolder                │   │
      │  │  ┌────────────────────────────────┐  │   │
      ├──┼─▶│        BlockingQueue           │──┼───┤
      │  │  │  ┌─────┐ ┌─────┐ ┌─────┐      │  │   │
      │  │  │  │Msg 1│ │Msg 2│ │Msg 3│ ...  │  │   │
      │  │  │  └─────┘ └─────┘ └─────┘      │  │   │
      │  │  └────────────────────────────────┘  │   │
      │  └──────────────────────────────────────┘   │
      │                                              │
   add()                                          pull()
```

### CacheMessage Structure

```cpp
class CacheMessage {
    std::string message_id_;           // Unique identifier
    std::shared_ptr<CacheData> data_;  // The actual data
};
```

### BlockingQueue

Thread-safe queue with:
- Blocking `Pop()` that waits for data
- Non-blocking `TryPop()`
- Timeout support via `PopWithTimeout()`
- ID-based retrieval via `PopById()`
- `Finish()` to signal no more data

```cpp
template <typename T>
    requires HasId<typename T::element_type>
class BlockingQueue {
    std::deque<T> queue_;
    std::mutex mutex_;
    std::condition_variable not_empty_;
    bool finished_;
};
```

---

## Memory Management

### Two-Tier Hierarchy

```
┌─────────────────────────────────────────────────────────┐
│                    L1: CPU Memory                        │
│                                                          │
│   ┌──────────┐  ┌──────────┐  ┌──────────┐             │
│   │CPUCache  │  │CPUCache  │  │CPUCache  │             │
│   │  Data    │  │  Data    │  │  Data    │             │
│   └──────────┘  └──────────┘  └──────────┘             │
│                                                          │
│   Saturation Threshold: 85%                             │
│   When exceeded: ────────────────────────┐              │
│                                          │ SPILL        │
└──────────────────────────────────────────┼──────────────┘
                                           │
                                           ▼
┌─────────────────────────────────────────────────────────┐
│                    L2: Disk Storage                      │
│                                                          │
│   /tmp/quest_cache/                                       │
│   ├── cache_0.cache                                     │
│   ├── cache_1.cache                                     │
│   └── cache_2.cache                                     │
│                                                          │
│   Saturation Threshold: 90%                             │
│   Cleanup: On destruction or explicit release           │
└─────────────────────────────────────────────────────────┘
```

### Memory Resource Interface

```cpp
class MemoryResource {
    virtual MemoryTier tier() const = 0;
    virtual MemoryStats stats() const = 0;
    virtual bool can_allocate(size_t bytes) const = 0;
    virtual bool is_saturated() const = 0;
};
```

### MemoryResourceManager

Coordinates between tiers:

```cpp
class MemoryResourceManager {
    MemoryTier recommend_tier(size_t bytes) const;
    bool should_spill() const;
};
```

### Automatic Spilling

When `BatchHolder::add()` is called with `auto_spill=true`:

```cpp
Status BatchHolder::add(CacheMessagePtr message) {
    if (auto_spill && memory_manager_->should_spill()) {
        auto disk_data = message->data()->to_disk();
        if (disk_data.has_value()) {
            message = make_message(disk_data.value());
        }
    }
    return queue_.Put(std::move(message));
}
```

---

## Execution Model

### Parallel Execution

The graph executes operators in parallel while respecting dependencies:

```cpp
void Graph::execute() {
    // 1. Find all source nodes (connected to sentinel)
    // 2. BFS traversal with dependency checking
    // 3. Launch each operator in its own thread
    // 4. Wait for all threads to complete
}
```

### Execution Flow

```
Time ─────────────────────────────────────────────────────▶

Thread 1:  ┌──────────────┐
           │  TableScan A │
           └──────┬───────┘
                  │ finish()
                  ▼

Thread 2:  ┌──────────────┐
           │  TableScan B │
           └──────┬───────┘
                  │ finish()
                  ▼

Thread 3:            ┌─────────────────────────┐
                     │         Join            │
                     │  (waits for both inputs)│
                     └────────────┬────────────┘
                                  │ finish()
                                  ▼

Thread 4:                         ┌──────────────┐
                                  │    Output    │
                                  └──────────────┘
```

### Flow Control

Operators can be throttled when downstream BatchHolders are saturated:

```cpp
void Operator::run() {
    while (has_input()) {
        auto batch = process(input());
        
        // Wait if output is full
        wait_if_output_saturated();
        
        add_to_output(batch);
    }
}
```

---

## Data Flow

### Complete Data Flow Example

```
Query: SELECT name, email FROM customers WHERE age > 21

┌─────────────────────────────────────────────────────────────────┐
│                         DATA FLOW                                │
└─────────────────────────────────────────────────────────────────┘

1. TableScan produces batches:
   
   ┌────────────┐     ┌─────────────────┐
   │ TableScan  │────▶│  BatchHolder 1  │
   │            │     │  [B1, B2, B3]   │
   └────────────┘     └────────┬────────┘
                               │
                               ▼
2. Filter consumes and produces:

   ┌────────────┐     ┌─────────────────┐
   │   Filter   │────▶│  BatchHolder 2  │
   │ (age > 21) │     │  [B1', B2']     │  (some rows filtered)
   └────────────┘     └────────┬────────┘
                               │
                               ▼
3. Project selects columns:

   ┌────────────┐     ┌─────────────────┐
   │  Project   │────▶│  BatchHolder 3  │
   │(name,email)│     │  [B1'', B2'']   │  (fewer columns)
   └────────────┘     └────────┬────────┘
                               │
                               ▼
4. Output collects results:

   ┌────────────┐
   │   Output   │────▶ Final Result
   └────────────┘
```

### Join Data Flow

```
┌────────────┐                           ┌────────────┐
│ TableScan  │                           │ TableScan  │
│ (customers)│                           │  (orders)  │
└─────┬──────┘                           └─────┬──────┘
      │                                        │
      ▼                                        ▼
┌───────────────┐                       ┌───────────────┐
│ BatchHolder   │                       │ BatchHolder   │
│   "input_a"   │                       │   "input_b"   │
└───────┬───────┘                       └───────┬───────┘
        │                                       │
        └──────────────┬────────────────────────┘
                       │
                       ▼
              ┌─────────────────┐
              │  PartwiseJoin   │
              │                 │
              │ 1. Pull all A   │
              │ 2. Pull all B   │
              │ 3. Join A × B   │
              └────────┬────────┘
                       │
                       ▼
              ┌─────────────────┐
              │   BatchHolder   │
              │  (joined data)  │
              └─────────────────┘
```

---

## Class Diagrams

### Core Classes

```
┌─────────────────────────────────────────────────────────────────┐
│                           Graph                                  │
├─────────────────────────────────────────────────────────────────┤
│ - operators_: vector<Operator*>                                 │
│ - edges_: map<int32_t, set<Edge>>                              │
│ - reverse_edges_: map<int32_t, set<Edge>>                      │
│ - memory_manager_: shared_ptr<MemoryResourceManager>           │
├─────────────────────────────────────────────────────────────────┤
│ + operator+=(KPair): KPair&                                     │
│ + add_node(Operator*): size_t                                   │
│ + add_edge(...): void                                           │
│ + execute(): Status                                             │
│ + get_neighbors(int32_t): set<Edge>                            │
└─────────────────────────────────────────────────────────────────┘
                              │
                              │ contains
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                         Operator                                 │
├─────────────────────────────────────────────────────────────────┤
│ - id_: uint32_t                                                 │
│ - expression_: string                                           │
│ - type_: OperatorType                                           │
│ - input_: Port                                                  │
│ - output_: Port                                                 │
├─────────────────────────────────────────────────────────────────┤
│ + run(): ExecutionStatus {abstract}                             │
│ + can_you_throttle_my_input(): bool {abstract}                 │
│ + input_cache(): shared_ptr<IBatchHolder>                      │
│ + output_cache(string): shared_ptr<IBatchHolder>               │
│ + add_to_output(CacheData): Status                             │
└─────────────────────────────────────────────────────────────────┘
                              │
                              │ has
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                           Port                                   │
├─────────────────────────────────────────────────────────────────┤
│ - batch_holders_: map<string, shared_ptr<IBatchHolder>>        │
│ - operator_: Operator*                                          │
├─────────────────────────────────────────────────────────────────┤
│ + register_port(string): void                                   │
│ + register_batch_holder(string, IBatchHolder): void            │
│ + get(string): shared_ptr<IBatchHolder>&                       │
│ + finish(): void                                                │
│ + all_finished(): bool                                          │
└─────────────────────────────────────────────────────────────────┘
```

### Cache Classes

```
┌─────────────────────────────────────────────────────────────────┐
│                      <<interface>>                               │
│                       IBatchHolder                               │
├─────────────────────────────────────────────────────────────────┤
│ + add(CacheMessagePtr): Status                                  │
│ + pull(): Result<CacheMessagePtr>                               │
│ + finish(): void                                                │
│ + is_finished(): bool                                           │
│ + batch_count(): size_t                                         │
│ + wait_if_saturated(): void                                     │
└─────────────────────────────────────────────────────────────────┘
                    △                     △
                    │                     │
         ┌─────────┴─────────┐   ┌───────┴────────┐
         │                   │   │                │
┌────────┴────────┐  ┌───────┴───────────────────┐
│   BatchHolder   │  │ ConcatenatingBatchHolder  │
├─────────────────┤  ├───────────────────────────┤
│ Simple FIFO     │  │ Merges all batches into   │
│ pull behavior   │  │ single result on pull     │
└─────────────────┘  └───────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                      <<abstract>>                                │
│                        CacheData                                 │
├─────────────────────────────────────────────────────────────────┤
│ + id(): string_view                                             │
│ + tier(): MemoryTier                                            │
│ + size_in_bytes(): size_t                                       │
│ + num_rows(): size_t                                            │
│ + to_cpu(): Result<shared_ptr<CacheData>>                      │
│ + to_disk(): Result<shared_ptr<CacheData>>                     │
│ + release(): void                                               │
└─────────────────────────────────────────────────────────────────┘
                    △                     △
                    │                     │
         ┌─────────┴─────────┐   ┌───────┴────────┐
         │                   │   │                │
┌────────┴────────┐  ┌───────┴───────────┐
│  CPUCacheData   │  │   DiskCacheData   │
├─────────────────┤  ├───────────────────┤
│ - data_: vector │  │ - file_path_: path│
│   <byte>        │  │ - size_bytes_     │
├─────────────────┤  ├───────────────────┤
│ In-memory data  │  │ File-based data   │
└─────────────────┘  └───────────────────┘
```

---

## Design Patterns

### 1. Producer-Consumer Pattern

Operators are connected via BatchHolders using the producer-consumer pattern:
- **Producer**: Calls `add()` to enqueue data
- **Consumer**: Calls `pull()` to dequeue data (blocks if empty)

### 2. Strategy Pattern

BatchHolder types (Simple vs Concatenating) implement different strategies for data retrieval.

### 3. Template Method Pattern

`SimpleOperator` provides a template `run()` method that calls the abstract `process_batch()`:

```cpp
ExecutionStatus SimpleOperator::run() {
    while (has_input()) {
        auto batch = pull_input();
        auto result = process_batch(batch);  // Abstract
        add_to_output(result);
    }
    return Proceed;
}
```

### 4. Fluent Interface

The DSL uses operator overloading for a fluent interface:

```cpp
graph += a >> b >> c;
```

### 5. RAII for Resource Management

- `CPUCacheData` tracks allocations and deallocations
- `DiskCacheData` deletes files on destruction
- `DiskMemoryResource` cleans up cache directory on destruction

---

## Extension Points

### Creating Custom Operators

```cpp
class MyOperator : public Operator {
public:
    MyOperator(uint32_t id)
        : Operator(id, "MyExpression", OperatorType::Project) {}
    
    bool can_you_throttle_my_input() const override {
        return true;
    }
    
    ExecutionStatus run() override {
        // Your implementation
        while (auto batch = input_cache()->pull()) {
            // Process batch
            add_to_output(result);
        }
        return ExecutionStatus::Proceed;
    }
};
```

### Creating Custom CacheData

```cpp
class NetworkCacheData : public CacheData {
public:
    MemoryTier tier() const override {
        return MemoryTier::Disk;  // Treat as remote storage
    }
    
    Result<shared_ptr<CacheData>> to_cpu() override {
        // Fetch from network, create CPUCacheData
    }
};
```

### Custom Memory Tiers

While QUEST currently supports CPU and Disk, the architecture can be extended:

```cpp
enum class MemoryTier : uint8_t {
    GPU = 0,   // Future: GPU memory
    CPU = 1,   // Current: Primary tier
    Disk = 2,  // Current: Secondary tier
    Network = 3 // Future: Remote storage
};
```

---

## Performance Considerations

### Memory Efficiency

1. **Automatic spilling** prevents OOM conditions
2. **Flow control** limits queue sizes
3. **Move semantics** minimize copies

### Concurrency

1. **Per-operator threads** enable parallel execution
2. **Lock-free statistics** using atomics
3. **Condition variables** for efficient blocking

### I/O Optimization

1. **Sequential file I/O** for disk cache
2. **Configurable cache directory** for SSD placement
3. **Cleanup on destruction** prevents disk bloat

---

## Future Enhancements

1. **GPU Memory Tier** - Add CUDA-based GPUCacheData
2. **Network Tier** - Distributed execution support
3. **Compression** - Compress data before spilling to disk
4. **Memory Pools** - Reduce allocation overhead
5. **Query Optimization** - Push-down predicates, join reordering
6. **Checkpointing** - Resume failed queries
7. **Metrics/Tracing** - OpenTelemetry integration