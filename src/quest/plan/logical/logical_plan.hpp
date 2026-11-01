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
  Scan(std::string path, std::shared_ptr<DataSource> data_source,
       std::optional<std::vector<std::string>> projection = std::nullopt)
      : path_(std::move(path)),
        data_source_(std::move(data_source)),
        projection_(std::move(projection)),
        schema_(DeriveSchema()) {}

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
  [[nodiscard]] Schema DeriveSchema() const;

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

/// @brief Whether two schemas are UNION-compatible (same field count + types).
[[nodiscard]] bool UnionCompatible(const Schema& lhs, const Schema& rhs);

}  // namespace quest::plan::logical
