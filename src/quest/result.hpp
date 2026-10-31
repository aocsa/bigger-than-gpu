#ifndef QUEST_RESULT_HPP_
#define QUEST_RESULT_HPP_

/// @file result.hpp
/// @brief Result<T> and Status error-handling types built on std::expected.

#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <stdexcept>
#include <concepts>
#include <source_location>
#include <type_traits>


namespace quest {

// =============================================================================
// Error Codes
// =============================================================================

enum class ErrorCode : uint8_t {
  // No error (sentinel value for Status::code() when OK).
  None = 0,

  // Invalid argument or malformed input data.
  Invalid,

  // File or network I/O error.
  IOError,

  // Memory allocation failed.
  OutOfMemory,

  // Resource (file, column, field) not found.
  NotFound,

  // Resource already exists (duplicate).
  AlreadyExists,

  // Operation was cancelled.
  Cancelled,

  // Unknown or unclassified error.
  Unknown,

  // Parquet-specific: Invalid or corrupt file format.
  ParquetFormatError,

  // Parquet-specific: Unsupported encoding type.
  UnsupportedEncoding,

  // Parquet-specific: Thrift protocol decoding error.
  ThriftDecodeError,

  // Parquet-specific: Page decoding error.
  PageDecodeError,

  // Parquet-specific: Column decoding error.
  ColumnDecodeError,

  // Type mismatch error (e.g., wrong column type).
  TypeError,

  // Index or offset out of valid range.
  OutOfRange,

  // End of data reached (used internally for iteration).
  EndOfData,

  // Query engine: Logical plan error.
  LogicalPlanError,

  // Query engine: Physical plan error.
  PhysicalPlanError,

  // Query engine: Expression evaluation error.
  ExpressionError,

  // Query engine: Schema mismatch or column not found.
  SchemaError
};

[[nodiscard]] constexpr std::string_view ErrorCodeName(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::None:
      return "None";
    case ErrorCode::Invalid:
      return "Invalid";
    case ErrorCode::IOError:
      return "IOError";
    case ErrorCode::OutOfMemory:
      return "OutOfMemory";
    case ErrorCode::NotFound:
      return "NotFound";
    case ErrorCode::AlreadyExists:
      return "AlreadyExists";
    case ErrorCode::Cancelled:
      return "Cancelled";
    case ErrorCode::Unknown:
      return "Unknown";
    case ErrorCode::ParquetFormatError:
      return "ParquetFormatError";
    case ErrorCode::UnsupportedEncoding:
      return "UnsupportedEncoding";
    case ErrorCode::ThriftDecodeError:
      return "ThriftDecodeError";
    case ErrorCode::PageDecodeError:
      return "PageDecodeError";
    case ErrorCode::ColumnDecodeError:
      return "ColumnDecodeError";
    case ErrorCode::TypeError:
      return "TypeError";
    case ErrorCode::OutOfRange:
      return "OutOfRange";
    case ErrorCode::EndOfData:
      return "EndOfData";
    case ErrorCode::LogicalPlanError:
      return "LogicalPlanError";
    case ErrorCode::PhysicalPlanError:
      return "PhysicalPlanError";
    case ErrorCode::ExpressionError:
      return "ExpressionError";
    case ErrorCode::SchemaError:
      return "SchemaError";
  }
  return "Unknown";
}

// source_location captures call-site automatically — no manual __FILE__/__LINE__ needed.
struct Error {
  ErrorCode code_;
  std::string message_;
  std::source_location loc_;

  Error(ErrorCode code, std::string message,
        std::source_location location = std::source_location::current())
      : code_(code), message_(std::move(message)), loc_(location) {}

  Error(ErrorCode code, const char* message,
        std::source_location location = std::source_location::current())
      : code_(code), message_(message != nullptr ? message : ""), loc_(location) {}

  Error(ErrorCode code, std::string_view message,
        std::source_location location = std::source_location::current())
      : code_(code), message_(message), loc_(location) {}

  Error(const Error& other) = default;
  Error(Error&& other) noexcept = default;
  Error& operator=(const Error& other) = default;
  Error& operator=(Error&& other) noexcept = default;
  ~Error() = default;

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] const std::source_location& location() const noexcept { return loc_; }

  [[nodiscard]] std::string ToString() const {
    return std::format("[{}:{}] {}: {}", loc_.file_name(), loc_.line(), ErrorCodeName(code_),
                       message_);
  }

  friend std::ostream& operator<<(std::ostream& out, const Error& error) {
    return out << error.ToString();
  }

  [[nodiscard]] explicit operator const char*() const { return message_.c_str(); }
  [[nodiscard]] explicit operator char*() { return (char*)message_.data(); }

  // Comparison is by code only — two errors with the same code are considered equal.
  [[nodiscard]] bool operator==(const Error& other) const noexcept { return code_ == other.code_; }
  [[nodiscard]] bool operator!=(const Error& other) const noexcept { return code_ != other.code_; }
  [[nodiscard]] bool operator==(ErrorCode code) const noexcept { return code_ == code; }
  [[nodiscard]] bool operator!=(ErrorCode code) const noexcept { return code_ != code; }
};

// =============================================================================
// Status Class
// =============================================================================

/// @brief Wraps std::expected<void, Error>; factory methods create typed errors.
class Status : public std::expected<void, Error> {
 public:
  using std::expected<void, Error>::expected;

  [[nodiscard]] static Status OK() noexcept { return {}; }

  [[nodiscard]] static std::unexpected<Error> Invalid(
      std::string message, std::source_location loc = std::source_location::current()) {
    return std::unexpected<Error>{Error(ErrorCode::Invalid, std::move(message), loc)};
  }

  [[nodiscard]] static std::unexpected<Error> IOError(
      std::string message, std::source_location loc = std::source_location::current()) {
    return std::unexpected<Error>{Error(ErrorCode::IOError, std::move(message), loc)};
  }
  [[nodiscard]] static std::unexpected<Error> OutOfMemory(
      std::string message, std::source_location loc = std::source_location::current()) {
    return std::unexpected<Error>{Error(ErrorCode::OutOfMemory, std::move(message), loc)};
  }
  [[nodiscard]] static std::unexpected<Error> NotFound(
      std::string message, std::source_location loc = std::source_location::current()) {
    return std::unexpected<Error>{Error(ErrorCode::NotFound, std::move(message), loc)};
  }
  [[nodiscard]] static std::unexpected<Error> AlreadyExists(
      std::string message, std::source_location loc = std::source_location::current()) {
    return std::unexpected<Error>{Error(ErrorCode::AlreadyExists, std::move(message), loc)};
  }
  [[nodiscard]] static std::unexpected<Error> Cancelled(
      std::string message, std::source_location loc = std::source_location::current()) {
    return std::unexpected<Error>{Error(ErrorCode::Cancelled, std::move(message), loc)};
  }
  [[nodiscard]] static std::unexpected<Error> ParquetFormatError(
      std::string message, std::source_location loc = std::source_location::current()) {
    return std::unexpected<Error>{Error(ErrorCode::ParquetFormatError, std::move(message), loc)};
  }
  [[nodiscard]] static std::unexpected<Error> UnsupportedEncoding(
      std::string message, std::source_location loc = std::source_location::current()) {
    return std::unexpected<Error>{Error(ErrorCode::UnsupportedEncoding, std::move(message), loc)};
  }
  [[nodiscard]] static std::unexpected<Error> ThriftDecodeError(
      std::string message, std::source_location loc = std::source_location::current()) {
    return std::unexpected<Error>{Error(ErrorCode::ThriftDecodeError, std::move(message), loc)};
  }
  [[nodiscard]] static std::unexpected<Error> PageDecodeError(
      std::string message, std::source_location loc = std::source_location::current()) {
    return std::unexpected<Error>{Error(ErrorCode::PageDecodeError, std::move(message), loc)};
  }
  [[nodiscard]] static std::unexpected<Error> ColumnDecodeError(
      std::string message, std::source_location loc = std::source_location::current()) {
    return std::unexpected<Error>{Error(ErrorCode::ColumnDecodeError, std::move(message), loc)};
  }
  [[nodiscard]] static std::unexpected<Error> TypeError(
      std::string message, std::source_location loc = std::source_location::current()) {
    return std::unexpected<Error>{Error(ErrorCode::TypeError, std::move(message), loc)};
  }
  [[nodiscard]] static std::unexpected<Error> OutOfRange(
      std::string message, std::source_location loc = std::source_location::current()) {
    return std::unexpected<Error>{Error(ErrorCode::OutOfRange, std::move(message), loc)};
  }
  /// @brief Sentinel used internally by iterators to signal end of sequence.
  [[nodiscard]] static std::unexpected<Error> EndOfData(
      std::source_location loc = std::source_location::current()) {
    return std::unexpected<Error>{Error(ErrorCode::EndOfData, std::string{}, loc)};
  }
  [[nodiscard]] static std::unexpected<Error> LogicalPlanError(
      std::string message, std::source_location loc = std::source_location::current()) {
    return std::unexpected<Error>{Error(ErrorCode::LogicalPlanError, std::move(message), loc)};
  }
  [[nodiscard]] static std::unexpected<Error> PhysicalPlanError(
      std::string message, std::source_location loc = std::source_location::current()) {
    return std::unexpected<Error>{Error(ErrorCode::PhysicalPlanError, std::move(message), loc)};
  }
  [[nodiscard]] static std::unexpected<Error> ExpressionError(
      std::string message, std::source_location loc = std::source_location::current()) {
    return std::unexpected<Error>{Error(ErrorCode::ExpressionError, std::move(message), loc)};
  }
  [[nodiscard]] static std::unexpected<Error> SchemaError(
      std::string message, std::source_location loc = std::source_location::current()) {
    return std::unexpected<Error>{Error(ErrorCode::SchemaError, std::move(message), loc)};
  }

  [[nodiscard]] bool ok() const noexcept { return this->has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] ErrorCode code() const noexcept {
    return this->has_value() ? ErrorCode::None : this->error().code_;
  }
  [[nodiscard]] std::string_view message() const noexcept {
    return this->has_value() ? std::string_view{} : this->error().message_;
  }
  [[nodiscard]] bool is_end_of_data() const noexcept {
    return !this->has_value() && this->error().code_ == ErrorCode::EndOfData;
  }

  [[nodiscard]] std::string ToString() const {
    if (ok()) {
      return "OK";
    }
    return error().ToString();
  }

};

// =============================================================================
// Result Type
// =============================================================================

/// @brief Result type using std::expected (C++23).
///
/// Result<T> holds either a value of type T on success, or a Status on failure.
/// Use has_value() to check success, value() to get the result, and error()
/// to get the Status on failure.
///
/// @tparam T The success value type.
template <typename T>
using Result = std::expected<T, Error>;

// =============================================================================
// Helper Functions
// =============================================================================

template <typename T>
[[nodiscard]] Result<T> Ok(T value) {
  return Result<T>{std::in_place, std::move(value)};
}

[[nodiscard]] inline std::unexpected<Error> Err(
    ErrorCode code, std::string message,
    std::source_location loc = std::source_location::current()) {
  return std::unexpected<Error>{Error{code, std::move(message), loc}};
}

[[nodiscard]] inline std::unexpected<Error> Err(const Error& error) {
  return std::unexpected<Error>{error};
}

}// namespace quest




// --- Macros ---

// QASSERT - Return Invalid status with formatted message if condition fails
#define QASSERT(condition, ...)                                 \
  do {                                                           \
    if (!(condition)) [[unlikely]] {                             \
      return ::quest::Status::Invalid(std::format(__VA_ARGS__)); \
    }                                                            \
  } while (false)

// =============================================================================
// Modern GUARD_ASSIGN macro (replaces QTRY)
// =============================================================================

// Internal helper to generate unique variable names
#define QUEST_CONCAT_IMPL(x, y) x##y
#define QUEST_CONCAT(x, y) QUEST_CONCAT_IMPL(x, y)
#define QUEST_UNIQUE_VAR(prefix) QUEST_CONCAT(prefix, __COUNTER__)

// GUARD_ASSIGN - Assign value or return error
// Usage: GUARD_ASSIGN(auto value, SomeOperation());
// Usage: GUARD_ASSIGN(existing_var, SomeOperation());
//
// This macro extracts the value from a Result<T> or Status and assigns it to lhs.
// If the result contains an error, it propagates the error by returning early.
// Compatible with std::expected and works without compiler extensions.
#define GUARD_ASSIGN(lhs, rexpr)                                      \
    GUARD_ASSIGN_IMPL(QUEST_UNIQUE_VAR(_result_), lhs, rexpr)

#define GUARD_ASSIGN_IMPL(result_var, lhs, rexpr)                     \
    auto result_var = (rexpr);                                                 \
    if (!result_var) [[unlikely]] {                                            \
        return std::unexpected(result_var.error());                            \
    }                                                                          \
    lhs = std::move(*result_var)

// GUARD_OK - Return early if status is not OK
// Usage: GUARD_OK(SomeOperation());
// For Status/Result<void> types where you don't need to extract a value
#define GUARD_OK(expr)                                              \
    do {                                                                       \
        auto _status_result_ = (expr);                                         \
        if (!_status_result_) [[unlikely]] {                                   \
            return std::unexpected(_status_result_.error());                   \
        }                                                                      \
    } while (false)


#endif  // QUEST_RESULT_HPP_
