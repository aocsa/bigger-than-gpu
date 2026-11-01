// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file logical_plan.hpp
/// @brief Logical plan nodes for query execution.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "quest/common/result.hpp"
#include "quest/datasource/data_source.hpp"
#include "quest/plan/logical/logical_expr.hpp"

namespace quest::plan::logical {

using datasource::DataSource;
using columnar::Schema;

// =============================================================================
// Abstract Base Class
// =============================================================================

/// @brief Abstract base class for logical plan nodes.
class LogicalPlan {
 public:
  virtual ~LogicalPlan() = default;

  [[nodiscard]] virtual Schema schema() const = 0;
  [[nodiscard]] virtual std::vector<const LogicalPlan*> Children() const = 0;
  [[nodiscard]] virtual std::string ToString() const = 0;

  /// @brief Pretty-prints the full plan subtree with indentation.
  [[nodiscard]] std::string Pretty(size_t indent = 0) const;
};

using LogicalPlanPtr = std::unique_ptr<LogicalPlan>;

// =============================================================================
// Scan Node
// =============================================================================

class Scan : public LogicalPlan {
 public:
  Scan(std::string path, std::shared_ptr<DataSource> data_source)
      : path_(std::move(path)),
        data_source_(std::move(data_source)),
        schema_(data_source_->schema()) {}

  /// @brief Scan reading only `columns`; SchemaError if one is not in the source.
  [[nodiscard]] static Result<std::unique_ptr<Scan>> WithProjection(
      std::string path, std::shared_ptr<DataSource> data_source,
      std::vector<std::string> columns);

  [[nodiscard]] Schema schema() const override { return schema_; }

  [[nodiscard]] std::vector<const LogicalPlan*> Children() const override {
    return {};
  }

  [[nodiscard]] std::string ToString() const override;

  [[nodiscard]] const std::string& path() const { return path_; }
  [[nodiscard]] const std::shared_ptr<DataSource>& data_source() const {
    return data_source_;
  }
  [[nodiscard]] const std::optional<std::vector<std::string>>& projection()
      const {
    return projection_;
  }

 private:
  // Passkey: only WithProjection can build a projected Scan, after validating the columns.
  struct Validated {
    explicit Validated() = default;
  };

 public:
  Scan(Validated, std::string path, std::shared_ptr<DataSource> data_source,
       std::vector<std::string> columns, Schema schema)
      : path_(std::move(path)),
        data_source_(std::move(data_source)),
        projection_(std::move(columns)),
        schema_(std::move(schema)) {}

 private:
  std::string path_;
  std::shared_ptr<DataSource> data_source_;
  std::optional<std::vector<std::string>> projection_;
  Schema schema_;
};

// =============================================================================
// Selection Node
// =============================================================================

class Selection : public LogicalPlan {
 public:
  Selection(LogicalPlanPtr input, LogicalExprPtr condition)
      : input_(std::move(input)), condition_(std::move(condition)) {}

  [[nodiscard]] Schema schema() const override { return input_->schema(); }

  [[nodiscard]] std::vector<const LogicalPlan*> Children() const override {
    return {input_.get()};
  }

  [[nodiscard]] std::string ToString() const override {
    return "Selection: " + condition_->ToString();
  }

  [[nodiscard]] const LogicalPlan& input() const { return *input_; }
  [[nodiscard]] const LogicalExpr& condition() const { return *condition_; }

 private:
  LogicalPlanPtr input_;
  LogicalExprPtr condition_;
};

// =============================================================================
// Projection Node
// =============================================================================

class Projection : public LogicalPlan {
 public:
  Projection(LogicalPlanPtr input, std::vector<LogicalExprPtr> expressions)
      : input_(std::move(input)), expressions_(std::move(expressions)) {}

  [[nodiscard]] Schema schema() const override;

  [[nodiscard]] std::vector<const LogicalPlan*> Children() const override {
    return {input_.get()};
  }

  [[nodiscard]] std::string ToString() const override;

  [[nodiscard]] const LogicalPlan& input() const { return *input_; }
  [[nodiscard]] const std::vector<LogicalExprPtr>& expressions() const {
    return expressions_;
  }

 private:
  LogicalPlanPtr input_;
  std::vector<LogicalExprPtr> expressions_;
};

// =============================================================================
// Limit Node
// =============================================================================

/// @brief Limit node - truncates the stream to the first @p limit rows.
class Limit : public LogicalPlan {
 public:
  Limit(LogicalPlanPtr input, std::size_t limit)
      : input_(std::move(input)), limit_(limit) {}

  [[nodiscard]] Schema schema() const override { return input_->schema(); }

  [[nodiscard]] std::vector<const LogicalPlan*> Children() const override {
    return {input_.get()};
  }

  [[nodiscard]] std::string ToString() const override {
    return "Limit: " + std::to_string(limit_);
  }

  [[nodiscard]] const LogicalPlan& input() const { return *input_; }
  [[nodiscard]] std::size_t limit() const noexcept { return limit_; }

 private:
  LogicalPlanPtr input_;
  std::size_t limit_;
};

// =============================================================================
// Union Node
// =============================================================================

/// @brief Union node - concatenates two input streams (UNION ALL semantics).
///
/// The first multi-child logical node. Both inputs must share a schema; the
/// gate/physical layer rejects a mismatch. Output schema is the left input's.
class Union : public LogicalPlan {
 public:
  Union(LogicalPlanPtr left, LogicalPlanPtr right)
      : left_(std::move(left)), right_(std::move(right)) {}

  [[nodiscard]] Schema schema() const override { return left_->schema(); }

  [[nodiscard]] std::vector<const LogicalPlan*> Children() const override {
    return {left_.get(), right_.get()};
  }

  [[nodiscard]] std::string ToString() const override { return "Union All"; }

  [[nodiscard]] const LogicalPlan& left() const { return *left_; }
  [[nodiscard]] const LogicalPlan& right() const { return *right_; }

 private:
  LogicalPlanPtr left_;
  LogicalPlanPtr right_;
};

// =============================================================================
// Join Node
// =============================================================================

/// @brief Inner equi-join on one key column from each side.
///
/// The build side is materialized into a hash table; the probe side streams.
/// Output schema: the probe columns, then the build columns.
class Join : public LogicalPlan {
 public:
  Join(LogicalPlanPtr probe, LogicalPlanPtr build, std::string probe_key, std::string build_key)
      : probe_(std::move(probe)),
        build_(std::move(build)),
        probe_key_(std::move(probe_key)),
        build_key_(std::move(build_key)) {}

  [[nodiscard]] Schema schema() const override;

  [[nodiscard]] std::vector<const LogicalPlan*> Children() const override {
    return {probe_.get(), build_.get()};
  }

  [[nodiscard]] std::string ToString() const override {
    return "Inner Join: " + probe_key_ + " = " + build_key_;
  }

  [[nodiscard]] const LogicalPlan& probe() const { return *probe_; }
  [[nodiscard]] const LogicalPlan& build() const { return *build_; }
  [[nodiscard]] const std::string& probe_key() const noexcept { return probe_key_; }
  [[nodiscard]] const std::string& build_key() const noexcept { return build_key_; }

 private:
  LogicalPlanPtr probe_;
  LogicalPlanPtr build_;
  std::string probe_key_;
  std::string build_key_;
};

// =============================================================================
// Aggregate Node
// =============================================================================

/// @brief One output column of an Aggregate: an aggregate expression and its name.
struct NamedAggregate {
  std::string name;
  LogicalExprPtr expr;  ///< SUM / COUNT / COUNT(*) / MIN / MAX / AVG.
};

/// @brief GROUP BY: one output row per distinct key. Output schema: the group
///        columns, then one column per aggregate.
class Aggregate : public LogicalPlan {
 public:
  Aggregate(LogicalPlanPtr input, std::vector<std::string> group_by,
            std::vector<NamedAggregate> aggregates)
      : input_(std::move(input)),
        group_by_(std::move(group_by)),
        aggregates_(std::move(aggregates)) {}

  [[nodiscard]] Schema schema() const override;

  [[nodiscard]] std::vector<const LogicalPlan*> Children() const override {
    return {input_.get()};
  }

  [[nodiscard]] std::string ToString() const override;

  [[nodiscard]] const LogicalPlan& input() const { return *input_; }
  [[nodiscard]] const std::vector<std::string>& group_by() const noexcept { return group_by_; }
  [[nodiscard]] const std::vector<NamedAggregate>& aggregates() const noexcept {
    return aggregates_;
  }

 private:
  LogicalPlanPtr input_;
  std::vector<std::string> group_by_;
  std::vector<NamedAggregate> aggregates_;
};

// =============================================================================
// Sort Node
// =============================================================================

struct SortKey {
  std::string column;
  bool ascending = true;
};

/// @brief ORDER BY: a stable sort on @p keys; NULLs sort last in both directions (DuckDB).
class Sort : public LogicalPlan {
 public:
  Sort(LogicalPlanPtr input, std::vector<SortKey> keys)
      : input_(std::move(input)), keys_(std::move(keys)) {}

  [[nodiscard]] Schema schema() const override { return input_->schema(); }

  [[nodiscard]] std::vector<const LogicalPlan*> Children() const override {
    return {input_.get()};
  }

  [[nodiscard]] std::string ToString() const override;

  [[nodiscard]] const LogicalPlan& input() const { return *input_; }
  [[nodiscard]] const std::vector<SortKey>& keys() const noexcept { return keys_; }

 private:
  LogicalPlanPtr input_;
  std::vector<SortKey> keys_;
};

/// @brief Whether two schemas are UNION-compatible (same field count + types).
[[nodiscard]] bool UnionCompatible(const Schema& lhs, const Schema& rhs);

}  // namespace quest::plan::logical
