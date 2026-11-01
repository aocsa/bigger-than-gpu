// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/plan/physical/expressions/physical_expr.hpp"

#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <string>

namespace quest::plan::physical {

namespace {

using columnar::ColumnBuilder;
using columnar::ColumnView;
using columnar::Decimal64;
using columnar::Int128;
using columnar::PowerOfTen;

/// @brief A one-row column holding @p value.
template <typename T>
[[nodiscard]] ColumnVectorPtr OneRow(DataType type, T value) {
  ColumnBuilder<T> builder(type, 1);
  builder.Append(value);
  return std::move(builder).Finish();
}

/// @brief True for a NULL literal: only literals are constant columns.
[[nodiscard]] bool IsNullLiteral(const ColumnVector& col) {
  return col.is_constant() && col.IsNull(0);
}

/// @brief @p n nulls of @p type: a computed result, not a constant, so IsNullLiteral (and with
/// it the typing of a NULL operand) sees only real NULL literals.
[[nodiscard]] ColumnVectorPtr AllNull(DataType type, size_t n) {
  return columnar::VisitType(type, [&]<typename Tag>(Tag) {
    ColumnBuilder<typename Tag::CType> builder(type, n);
    for (size_t i = 0; i < n; ++i) {
      builder.AppendNull();
    }
    return std::move(builder).Finish();
  });
}

[[nodiscard]] Result<DataType> OperandResultType(BinaryOp op, const ColumnVector& l,
                                                 const ColumnVector& r) {
  return BinaryResultType(op, IsNullLiteral(l) ? r.GetType() : l.GetType(),
                          IsNullLiteral(r) ? l.GetType() : r.GetType());
}

// =============================================================================
// Cast
// =============================================================================

/// @brief A numeric C++ type a Cast reads or writes: an integer, a float or a Decimal.
template <typename T>
constexpr bool kIsNumber = (std::is_arithmetic_v<T> && !std::same_as<T, bool>) ||
                           std::same_as<T, Decimal64>;

/// @brief One value of @p from (a Decimal at @p from_scale) as @p To (a Decimal at @p to_scale).
template <typename To, typename From>
[[nodiscard]] Result<To> CastOne(From from, int from_scale, int to_scale) {
  if constexpr (std::same_as<To, Decimal64>) {
    std::optional<std::int64_t> unscaled;
    if constexpr (std::same_as<From, Decimal64>) {
      unscaled = to_scale >= from_scale ? columnar::Rescale(from.unscaled, from_scale, to_scale)
                                        : std::nullopt;
    } else if constexpr (std::is_integral_v<From>) {
      unscaled = columnar::Rescale(static_cast<std::int64_t>(from), 0, to_scale);
    }
    if (!unscaled) {
      return Error::OutOfRange("Value out of range in a cast to Decimal of scale " +
                               std::to_string(to_scale));
    }
    return Decimal64{*unscaled};
  } else if constexpr (std::same_as<From, Decimal64>) {
    return static_cast<To>(static_cast<double>(from.unscaled) /
                           static_cast<double>(PowerOfTen(from_scale)));
  } else {
    return static_cast<To>(from);
  }
}

/// @brief @p col as @p type: a number widens, a Decimal rescales up, a float becomes no
/// Decimal. An equal type comes back as it is; a constant column stays constant.
[[nodiscard]] Result<ColumnVectorPtr> Cast(const ColumnVectorPtr& col, DataType type) {
  if (col->GetType() == type) {
    return col;
  }
  if (IsNullLiteral(*col)) {
    return AllNull(type, col->size());
  }
  const DataType from_type = col->GetType();
  const size_t rows = col->is_constant() ? 1 : col->size();
  auto cast = columnar::VisitType(from_type, [&]<typename FromTag>(FromTag) {
    return columnar::VisitType(type, [&]<typename ToTag>(ToTag) -> Result<ColumnVectorPtr> {
      using From = typename FromTag::CType;
      using To = typename ToTag::CType;
      if constexpr ((kIsNumber<From> || std::same_as<From, bool>) && kIsNumber<To> &&
                    !(std::is_floating_point_v<From> && std::same_as<To, Decimal64>)) {
        const auto in = col->View<From>();
        ColumnBuilder<To> out(type, rows);
        for (size_t i = 0; i < rows; ++i) {
          if (in.IsNull(i)) {
            out.AppendNull();
            continue;
          }
          auto value = CastOne<To>(in[i], from_type.scale, type.scale);
          if (!value) {
            return std::unexpected(std::move(value).error());
          }
          out.Append(*value);
        }
        return std::move(out).Finish();
      } else {
        return Error::TypeError("Cannot cast " + columnar::GetTypeName(from_type) + " to " +
                                columnar::GetTypeName(type));
      }
    });
  });
  if (!cast || !col->is_constant()) {
    return cast;
  }
  return ColumnVector::Constant(**cast, col->size());
}

// =============================================================================
// Comparison and AND/OR
// =============================================================================

/// @brief `l op r` for each row; null where either side is null.
template <typename Op, typename T>
[[nodiscard]] ColumnVectorPtr CompareViews(ColumnView<T> l, ColumnView<T> r, size_t n) {
  ColumnBuilder<bool> out(columnar::DataType::kBool, n);
  if (!l.has_nulls() && !r.has_nulls()) {
    for (size_t i = 0; i < n; ++i) {
      out.Append(Op{}(l[i], r[i]));
    }
  } else {
    for (size_t i = 0; i < n; ++i) {
      l.IsNull(i) || r.IsNull(i) ? out.AppendNull() : out.Append(Op{}(l[i], r[i]));
    }
  }
  return std::move(out).Finish();
}

/// @brief `l op r` for Decimals of scales @p ls and @p rs: both at the larger scale, in 128
/// bits so the rescale cannot overflow.
template <typename Op>
[[nodiscard]] ColumnVectorPtr CompareDecimals(ColumnView<Decimal64> l, int ls,
                                              ColumnView<Decimal64> r, int rs, size_t n) {
  const int scale = std::max(ls, rs);
  const Int128 lf = PowerOfTen(scale - ls);
  const Int128 rf = PowerOfTen(scale - rs);
  ColumnBuilder<bool> out(columnar::DataType::kBool, n);
  for (size_t i = 0; i < n; ++i) {
    if (l.IsNull(i) || r.IsNull(i)) {
      out.AppendNull();
    } else {
      out.Append(Op{}(Int128{l[i].unscaled} * lf, Int128{r[i].unscaled} * rf));
    }
  }
  return std::move(out).Finish();
}

/// @brief SQL comparison: null if either side is null. Numbers compare in one type (exactly
/// for integers and Decimals); other mismatched types compare false.
template <typename Op>
[[nodiscard]] Result<ColumnVectorPtr> Compare(ColumnVectorPtr l, ColumnVectorPtr r, size_t n) {
  const DataType bool_type = columnar::DataType::kBool;
  if (IsNullLiteral(*l) || IsNullLiteral(*r)) {
    return AllNull(bool_type, n);
  }
  DataType lt = l->GetType();
  DataType rt = r->GetType();
  if (lt.id != rt.id || (lt != rt && !columnar::IsDecimal(lt))) {
    // A Bool meets an integer as an integer (true = 1), as in DuckDB.
    auto integer_like = [](DataType t) {
      return columnar::IsInteger(t) || t == DataType::kBool;
    };
    if (integer_like(lt) && integer_like(rt)) {
      auto lcast = Cast(l, DataType::kInt64);
      auto rcast = Cast(r, DataType::kInt64);
      if (!lcast || !rcast) {
        return std::unexpected(!lcast ? lcast.error() : rcast.error());
      }
      return CompareViews<Op, std::int64_t>((*lcast)->View<std::int64_t>(),
                                            (*rcast)->View<std::int64_t>(), n);
    }
    if (!columnar::IsNumeric(lt) || !columnar::IsNumeric(rt)) {
      ColumnBuilder<bool> out(bool_type, n);
      for (size_t i = 0; i < n; ++i) {
        l->IsNull(i) || r->IsNull(i) ? out.AppendNull() : out.Append(false);
      }
      return std::move(out).Finish();
    }
    const bool exact = !(lt.id == columnar::TypeId::kFloat || lt.id == columnar::TypeId::kDouble ||
                         rt.id == columnar::TypeId::kFloat || rt.id == columnar::TypeId::kDouble);
    const bool decimal = columnar::IsDecimal(lt) || columnar::IsDecimal(rt);
    // An integer meets a Decimal as Decimal(digits, 0); other mixes meet as Int64 or Double.
    const DataType lc = !exact ? DataType::kDouble : decimal ? AsDecimal(lt) : DataType::kInt64;
    const DataType rc = !exact ? DataType::kDouble : decimal ? AsDecimal(rt) : DataType::kInt64;
    auto lcast = Cast(l, lc);
    auto rcast = Cast(r, rc);
    if (!lcast || !rcast) {
      return std::unexpected(!lcast ? lcast.error() : rcast.error());
    }
    l = std::move(*lcast);
    r = std::move(*rcast);
    lt = lc;
    rt = rc;
  }
  if (columnar::IsDecimal(lt)) {
    return CompareDecimals<Op>(l->View<Decimal64>(), lt.scale, r->View<Decimal64>(), rt.scale, n);
  }
  return columnar::VisitType(lt, [&]<typename Tag>(Tag) -> Result<ColumnVectorPtr> {
    using T = typename Tag::CType;
    return CompareViews<Op, T>(l->View<T>(), r->View<T>(), n);
  });
}

/// @brief Three-valued AND/OR: the dominant value (false for AND, true for OR) wins over
/// null; otherwise any null makes the result null.
[[nodiscard]] Result<ColumnVectorPtr> AndOr(BinaryOp op, ColumnVectorPtr l, ColumnVectorPtr r,
                                            size_t n) {
  for (ColumnVectorPtr* side : {&l, &r}) {
    if (IsNullLiteral(**side)) {
      *side = AllNull(columnar::DataType::kBool, n);
    } else if ((*side)->GetType() != columnar::DataType::kBool) {
      return Error::TypeError("'" + std::string(plan::ToString(op)) + "' needs bool operands");
    }
  }
  const bool dominant = op == BinaryOp::kOr;
  const auto a = l->View<bool>();
  const auto b = r->View<bool>();
  ColumnBuilder<bool> out(columnar::DataType::kBool, n);
  for (size_t i = 0; i < n; ++i) {
    const bool a_null = a.IsNull(i);
    const bool b_null = b.IsNull(i);
    if ((!a_null && a[i] == dominant) || (!b_null && b[i] == dominant)) {
      out.Append(dominant);
    } else if (a_null || b_null) {
      out.AppendNull();
    } else {
      out.Append(!dominant);
    }
  }
  return std::move(out).Finish();
}

// =============================================================================
// Arithmetic
// =============================================================================

/// @brief `fn(l[i], r[i])` for each row; null where either side is null or fn gives nullopt.
template <typename Out, typename T, typename Fn>
[[nodiscard]] Result<ColumnVectorPtr> MapViews(DataType type, ColumnView<T> l, ColumnView<T> r,
                                               size_t n, Fn fn) {
  ColumnBuilder<Out> out(type, n);
  for (size_t i = 0; i < n; ++i) {
    if (l.IsNull(i) || r.IsNull(i)) {
      out.AppendNull();
      continue;
    }
    Result<std::optional<Out>> value = fn(l[i], r[i]);
    if (!value) {
      return std::unexpected(std::move(value).error());
    }
    *value ? out.Append(**value) : out.AppendNull();
  }
  return std::move(out).Finish();
}

[[nodiscard]] std::unexpected<Error> Overflow(BinaryOp op, std::string_view what) {
  return Error::OutOfRange(std::string(what) + " overflow in '" + std::string(plan::ToString(op)) +
                           "'");
}

/// @brief One row of `x op y` for an integer type: overflow is an error, `%` by zero is null.
template <typename T>
[[nodiscard]] Result<std::optional<T>> IntegerOp(BinaryOp op, T x, T y) {
  T out{};
  bool overflow = false;
  switch (op) {
    case BinaryOp::kAdd:
      overflow = __builtin_add_overflow(x, y, &out);
      break;
    case BinaryOp::kSubtract:
      overflow = __builtin_sub_overflow(x, y, &out);
      break;
    case BinaryOp::kMultiply:
      overflow = __builtin_mul_overflow(x, y, &out);
      break;
    case BinaryOp::kModulo:
      if (y == 0) {
        return std::nullopt;
      }
      // min % -1 overflows in C++; the mathematical result is 0.
      out = y == -1 ? T{0} : static_cast<T>(x % y);
      break;
    default:
      std::unreachable();  // integer `/` yields double
  }
  if (overflow) {
    return Overflow(op, "Integer");
  }
  return out;
}

/// @brief One row of `x op y` for a float type: `/` and `%` by zero are null.
template <typename T>
[[nodiscard]] std::optional<T> FloatingOp(BinaryOp op, T x, T y) {
  switch (op) {
    case BinaryOp::kAdd:
      return x + y;
    case BinaryOp::kSubtract:
      return x - y;
    case BinaryOp::kMultiply:
      return x * y;
    case BinaryOp::kDivide:
      return y == 0 ? std::optional<T>{} : x / y;
    case BinaryOp::kModulo:
      return y == 0 ? std::optional<T>{} : std::fmod(x, y);
    default:
      std::unreachable();
  }
}

/// @brief `l op r` in the result type of BinaryResultType: both operands are cast to it (for
/// `*` of Decimals, each keeps its scale and the unscaled values multiply), then one loop.
[[nodiscard]] Result<ColumnVectorPtr> Arithmetic(BinaryOp op, ColumnVectorPtr l,
                                                 ColumnVectorPtr r, size_t n) {
  auto type = OperandResultType(op, *l, *r);
  if (!type) {
    return std::unexpected(type.error());
  }
  if (IsNullLiteral(*l) || IsNullLiteral(*r)) {
    return AllNull(*type, n);
  }
  const bool decimal_product = columnar::IsDecimal(*type) && op == BinaryOp::kMultiply;
  auto lcast = Cast(l, decimal_product ? AsDecimal(l->GetType()) : *type);
  auto rcast = Cast(r, decimal_product ? AsDecimal(r->GetType()) : *type);
  if (!lcast || !rcast) {
    return std::unexpected(!lcast ? lcast.error() : rcast.error());
  }
  return columnar::VisitType(*type, [&]<typename Tag>(Tag) -> Result<ColumnVectorPtr> {
    using T = typename Tag::CType;
    const auto a = (*lcast)->View<T>();
    const auto b = (*rcast)->View<T>();
    if constexpr (std::same_as<T, Decimal64>) {
      return MapViews<T>(*type, a, b, n, [op](T x, T y) -> Result<std::optional<T>> {
        std::int64_t out = 0;
        const bool overflow =
            op == BinaryOp::kAdd        ? __builtin_add_overflow(x.unscaled, y.unscaled, &out)
            : op == BinaryOp::kSubtract ? __builtin_sub_overflow(x.unscaled, y.unscaled, &out)
                                        : __builtin_mul_overflow(x.unscaled, y.unscaled, &out);
        if (overflow) {
          return Overflow(op, "Decimal");
        }
        return T{out};
      });
    } else if constexpr (std::is_integral_v<T> && !std::same_as<T, bool>) {
      return MapViews<T>(*type, a, b, n, [op](T x, T y) { return IntegerOp(op, x, y); });
    } else if constexpr (std::is_floating_point_v<T>) {
      return MapViews<T>(*type, a, b, n,
                         [op](T x, T y) -> Result<std::optional<T>> { return FloatingOp(op, x, y); });
    } else {
      std::unreachable();  // BinaryResultType yields only numeric types for arithmetic
    }
  });
}

/// @brief Each row of a numeric @p col as double (a Decimal divided by 10^scale); 0 for a null
/// row or a non-numeric column.
[[nodiscard]] std::vector<double> AsDoubles(const ColumnVector& col) {
  std::vector<double> values(col.size(), 0.0);
  columnar::VisitType(col.GetType(), [&]<typename Tag>(Tag) {
    using T = typename Tag::CType;
    if constexpr (kIsNumber<T>) {
      const auto view = col.View<T>();
      for (size_t i = 0; i < values.size(); ++i) {
        if (!view.IsNull(i)) {
          auto value = CastOne<double>(view[i], col.GetType().scale, 0);
          values[i] = value ? *value : 0.0;
        }
      }
    }
  });
  return values;
}

}  // namespace

// =============================================================================
// Literal Expression
// =============================================================================

Result<ColumnVectorPtr> LiteralExpression::Evaluate(
    const RecordBatch& input) const {
  return ColumnVector::Constant(value_.column(), input.num_rows());
}

std::string LiteralExpression::ToString() const { return value_.ToSql(); }

// =============================================================================
// Column Expression
// =============================================================================

Result<ColumnVectorPtr> ColumnExpression::Evaluate(
    const RecordBatch& input) const {
  if (index_ >= input.num_columns()) {
    return std::unexpected(
        Error::OutOfRange("Column index out of range: " +
                           std::to_string(index_)));
  }
  return input.column_ptr(index_);
}

// =============================================================================
// Binary Expression
// =============================================================================

Result<ColumnVectorPtr> BinaryExpression::Evaluate(
    const RecordBatch& input) const {
  auto left_result = left_->Evaluate(input);
  if (!left_result) {
    return std::unexpected(left_result.error());
  }
  auto right_result = right_->Evaluate(input);
  if (!right_result) {
    return std::unexpected(right_result.error());
  }

  ColumnVectorPtr l = std::move(*left_result);
  ColumnVectorPtr r = std::move(*right_result);
  const size_t n = input.num_rows();

  auto compare = [&]<typename Op>() -> Result<ColumnVectorPtr> {
    if (auto type = OperandResultType(op_, *l, *r); !type) {
      return std::unexpected(type.error());
    }
    return Compare<Op>(l, r, n);
  };
  switch (op_) {
    case BinaryOp::kEq:
      return compare.template operator()<std::equal_to<>>();
    case BinaryOp::kNeq:
      return compare.template operator()<std::not_equal_to<>>();
    case BinaryOp::kLt:
      return compare.template operator()<std::less<>>();
    case BinaryOp::kLtEq:
      return compare.template operator()<std::less_equal<>>();
    case BinaryOp::kGt:
      return compare.template operator()<std::greater<>>();
    case BinaryOp::kGtEq:
      return compare.template operator()<std::greater_equal<>>();
    case BinaryOp::kAnd:
    case BinaryOp::kOr:
      return AndOr(op_, l, r, n);
    case BinaryOp::kAdd:
    case BinaryOp::kSubtract:
    case BinaryOp::kMultiply:
    case BinaryOp::kDivide:
    case BinaryOp::kModulo:
      return Arithmetic(op_, l, r, n);
  }
  std::unreachable();
}

std::string BinaryExpression::ToString() const {
  std::string s =
      left_->ToString() + " " + std::string(plan::ToString(op_)) + " " + right_->ToString();
  return IsArithmetic(op_) ? "(" + s + ")" : s;
}

// =============================================================================
// CASE Expression
// =============================================================================

Result<ColumnVectorPtr> CaseExpression::Evaluate(const RecordBatch& input) const {
  std::vector<ColumnVectorPtr> whens;
  std::vector<ColumnVectorPtr> results;
  whens.reserve(branches_.size());
  results.reserve(branches_.size() + 1);
  for (const auto& [when, then] : branches_) {
    auto condition = when->Evaluate(input);
    if (!condition) {
      return std::unexpected(condition.error());
    }
    const DataType condition_type = (*condition)->GetType();
    if (!IsNullLiteral(**condition) &&
        !(condition_type == columnar::DataType::kBool)) {
      return Error::TypeError("CASE WHEN needs a bool condition, got " +
                              columnar::GetTypeName(condition_type));
    }
    whens.push_back(std::move(*condition));
    auto result = then->Evaluate(input);
    if (!result) {
      return std::unexpected(result.error());
    }
    results.push_back(std::move(*result));
  }
  auto otherwise = otherwise_->Evaluate(input);
  if (!otherwise) {
    return std::unexpected(otherwise.error());
  }
  results.push_back(std::move(*otherwise));

  std::optional<DataType> type;
  for (const auto& result : results) {
    if (IsNullLiteral(*result)) {
      continue;
    }
    if (!type) {
      type = result->GetType();
      continue;
    }
    auto common = CommonType(*type, result->GetType());
    if (!common) {
      return std::unexpected(common.error());
    }
    type = std::move(*common);
  }
  if (!type) {
    type = results.back()->GetType();  // every result is a NULL literal
  }

  for (auto& result : results) {
    auto cast = Cast(result, *type);
    if (!cast) {
      return std::unexpected(cast.error());
    }
    result = std::move(*cast);
  }

  // A NULL-literal condition is never true.
  std::vector<std::optional<ColumnView<bool>>> conditions;
  conditions.reserve(whens.size());
  for (const auto& when : whens) {
    conditions.push_back(IsNullLiteral(*when) ? std::nullopt
                                              : std::optional(when->View<bool>()));
  }
  const size_t n = input.num_rows();
  return columnar::VisitType(*type, [&]<typename Tag>(Tag) -> Result<ColumnVectorPtr> {
    using T = typename Tag::CType;
    std::vector<ColumnView<T>> values;
    values.reserve(results.size());
    for (const auto& result : results) {
      values.push_back(result->View<T>());
    }
    ColumnBuilder<T> out(*type, n);
    for (size_t i = 0; i < n; ++i) {
      size_t pick = conditions.size();  // the ELSE
      for (size_t b = 0; b < conditions.size(); ++b) {
        if (conditions[b] && !conditions[b]->IsNull(i) && (*conditions[b])[i]) {
          pick = b;
          break;
        }
      }
      values[pick].IsNull(i) ? out.AppendNull() : out.Append(values[pick][i]);
    }
    return std::move(out).Finish();
  });
}

std::string CaseExpression::ToString() const {
  std::string s = "CASE";
  for (const auto& [when, then] : branches_) {
    s += " WHEN " + when->ToString() + " THEN " + then->ToString();
  }
  return s + " ELSE " + otherwise_->ToString() + " END";
}

// =============================================================================
// Aggregate Expressions
// =============================================================================

Result<ColumnVectorPtr> SumExpression::Evaluate(
    const RecordBatch& input) const {
  auto input_result = input_->Evaluate(input);
  if (!input_result) {
    return std::unexpected(input_result.error());
  }

  auto& col = **input_result;
  const std::vector<double> values = AsDoubles(col);
  double sum = 0.0;

  for (size_t i = 0; i < col.size(); ++i) {
    if (!col.IsNull(i)) {
      sum += values[i];
    }
  }

  return OneRow<double>(columnar::DataType::kDouble, sum);
}

Result<ColumnVectorPtr> AvgExpression::Evaluate(
    const RecordBatch& input) const {
  auto input_result = input_->Evaluate(input);
  if (!input_result) {
    return std::unexpected(input_result.error());
  }

  auto& col = **input_result;
  const std::vector<double> values = AsDoubles(col);
  double sum = 0.0;
  size_t count = 0;

  for (size_t i = 0; i < col.size(); ++i) {
    if (!col.IsNull(i)) {
      sum += values[i];
      ++count;
    }
  }

  double avg = (count > 0) ? (sum / static_cast<double>(count)) : 0.0;

  return OneRow<double>(columnar::DataType::kDouble, avg);
}

Result<ColumnVectorPtr> CountExpression::Evaluate(
    const RecordBatch& input) const {
  auto input_result = input_->Evaluate(input);
  if (!input_result) {
    return std::unexpected(input_result.error());
  }

  auto& col = **input_result;
  int64_t count = 0;

  for (size_t i = 0; i < col.size(); ++i) {
    if (!col.IsNull(i)) {
      ++count;
    }
  }

  return OneRow<int64_t>(columnar::DataType::kInt64, count);
}

Result<ColumnVectorPtr> CountStarExpression::Evaluate(
    const RecordBatch& input) const {
  return OneRow<int64_t>(columnar::DataType::kInt64, static_cast<int64_t>(input.num_rows()));
}

Result<ColumnVectorPtr> MinExpression::Evaluate(
    const RecordBatch& input) const {
  auto input_result = input_->Evaluate(input);
  if (!input_result) {
    return std::unexpected(input_result.error());
  }

  auto& col = **input_result;
  const std::vector<double> values = AsDoubles(col);
  if (col.size() == 0) {
    return OneRow<double>(columnar::DataType::kDouble, std::numeric_limits<double>::quiet_NaN());
  }

  double min_val = std::numeric_limits<double>::max();
  bool found = false;

  for (size_t i = 0; i < col.size(); ++i) {
    if (!col.IsNull(i)) {
      double val = values[i];
      if (!found || val < min_val) {
        min_val = val;
        found = true;
      }
    }
  }

  if (!found) {
    min_val = std::numeric_limits<double>::quiet_NaN();
  }

  return OneRow<double>(columnar::DataType::kDouble, min_val);
}

Result<ColumnVectorPtr> MaxExpression::Evaluate(
    const RecordBatch& input) const {
  auto input_result = input_->Evaluate(input);
  if (!input_result) {
    return std::unexpected(input_result.error());
  }

  auto& col = **input_result;
  const std::vector<double> values = AsDoubles(col);
  if (col.size() == 0) {
    return OneRow<double>(columnar::DataType::kDouble, std::numeric_limits<double>::quiet_NaN());
  }

  double max_val = std::numeric_limits<double>::lowest();
  bool found = false;

  for (size_t i = 0; i < col.size(); ++i) {
    if (!col.IsNull(i)) {
      double val = values[i];
      if (!found || val > max_val) {
        max_val = val;
        found = true;
      }
    }
  }

  if (!found) {
    max_val = std::numeric_limits<double>::quiet_NaN();
  }

  return OneRow<double>(columnar::DataType::kDouble, max_val);
}

}  // namespace quest::plan::physical
