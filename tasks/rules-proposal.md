# QUEST Code Rules Proposal

## Purpose

Codify the implicit conventions already followed across 35+ header files into explicit rules. This prevents drift across sessions and contributors.

## Findings

### Consistent Patterns (undocumented until now)

| Convention | Pattern | Example |
|---|---|---|
| Classes/Structs | PascalCase | `BatchHolder`, `DataBlock`, `Operator` |
| Methods | PascalCase | `Execute`, `ProcessBatch`, `AddNode` |
| Private members | snake_case + `_` | `plan_`, `input_port_`, `operator_id_` |
| Namespaces | snake_case | `quest::taskflow`, `quest::cache` |
| Files | snake_case | `batch_holder.hpp`, `operator_type.hpp` |
| Type aliases | PascalCase + Ptr/Type | `LogicalPlanPtr`, `DataBlockPtr` |
| Constants | `k`-prefix PascalCase | `kGraphEntryPointId` |
| `[[nodiscard]]` | Getters, query methods, Result returns | All files consistently |
| `noexcept` | Trivial getters, identity methods | All files consistently |
| Section banners | `// ====...====` + title | All newer files |
| Documentation | Doxygen `///` with `@brief/@param/@return` | `operator.hpp`, `graph.hpp`, `port.hpp` |

### Inconsistencies

#### 1. Header Guards (needs decision)

- **28 files**: `#ifndef QUEST_TASKFLOW_OPERATOR_HPP_` style
- **7 files**: `#pragma once` (cache/, memory/, result.hpp, quest.hpp, tests/)

**Recommendation**: Standardize on `#ifndef` (less churn, includes namespace path).

#### 2. Enum Value Naming (needs decision)

Two conventions coexist:
- **Parquet/datasource layer**: `k`-prefix → `kSimple`, `kBoolean`, `kPlain`, `kRequired`
- **Taskflow/execution layer**: PascalCase → `TableScan`, `Stop`, `Proceed`, `None`

**Recommendation**: Document both as intentional. Parquet enums mirror the spec. New non-Parquet enums use PascalCase.

#### 3. Copyright Headers

Three variants exist:
1. `// Copyright 2024 Quest Authors` + `// SPDX-License-Identifier: Apache-2.0`
2. `// Copyright 2024` + `// Licensed under the Apache License, Version 2.0`
3. No header

**Recommendation**: Standardize on SPDX short form for new files. Don't reformat existing.

#### 4. Doc Comments

- Newer files: Doxygen `///` with `@brief`, `@param`, `@return`, `@code`
- Older files (schema.hpp, types.hpp): plain `//` comments

**Recommendation**: New files MUST use Doxygen. Existing `//` comments are fine until that code is touched.

## Rules to Add to CLAUDE.md

All rules below codify what the codebase already does — no new conventions invented.

### Naming Conventions
- Classes/Structs: PascalCase (`BatchHolder`, `DataBlock`)
- Methods: PascalCase (`Execute`, `ProcessBatch`)
- Private members: snake_case with trailing `_` (`plan_`, `input_port_`)
- Namespaces: snake_case (`quest::taskflow`, `quest::cache`)
- Files: snake_case (`batch_holder.hpp`, `operator_type.hpp`)
- Type aliases: PascalCase with Ptr/Type suffix (`LogicalPlanPtr`, `DataBlockPtr`)
- Constants: `k`-prefix PascalCase (`kGraphEntryPointId`)
- Enum values: PascalCase for execution layer, `k`-prefix for Parquet/datasource layer

### Documentation
- Public API: Doxygen `///` with `@brief`, `@param`, `@return`
- Usage examples: `@code`/`@endcode` blocks
- Section separators: `// ====...====` banners
- New files MUST use Doxygen. Don't reformat existing `//` comments unless touching that code.

### Header Guards
- Use `#ifndef`/`#define`/`#endif` style
- Pattern: `QUEST_{SUBSYSTEM}_{FILENAME}_HPP_` (e.g., `QUEST_TASKFLOW_OPERATOR_HPP_`)
- Do NOT use `#pragma once` in new files

### Build & Test
- Build: `cmake --build build`
- Test all: `ctest --test-dir build --output-on-failure`
- Test one: `./build/tests/test_{name}`
- Test filter: `./build/tests/test_{name} --gtest_filter="*Pattern*"`
- Clean build: `rm -rf build && cmake -B build && cmake --build build`

### Code Quality Markers
- `[[nodiscard]]` on ALL: getters, query methods, factory functions, Result-returning functions
- `noexcept` on: trivial getters, identity methods, moved-from state checks
- `const` correctness: const-qualify all methods that don't mutate state

### Copyright
- New files: SPDX short form (`// Copyright 2024 Alexander Ocsa` + `// SPDX-License-Identifier: Apache-2.0`)
- Existing files: don't reformat unless touching
