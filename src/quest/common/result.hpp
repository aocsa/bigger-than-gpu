#pragma once

/// @file result.hpp
/// @brief Result<T> and Status error-handling types built on std::expected.

#include <array>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <source_location>


namespace quest {

// =============================================================================
// Error Codes
// =============================================================================

enum class ErrorCode : uint8_t {
  // No error.
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

  // Query engine: Logical plan error.
  LogicalPlanError,

  // Query engine: Physical plan error.
  PhysicalPlanError,

  // Query engine: Expression evaluation error.
  ExpressionError,

  // Query engine: Schema mismatch or column not found.
  SchemaError
};

inline constexpr std::array<std::string_view, 19> kErrorCodeNames = {
    "None", "Invalid", "IOError", "OutOfMemory", "NotFound", "AlreadyExists", "Cancelled",
    "Unknown", "ParquetFormatError", "UnsupportedEncoding", "ThriftDecodeError", "PageDecodeError",
    "ColumnDecodeError", "TypeError", "OutOfRange", "LogicalPlanError", "PhysicalPlanError",
    "ExpressionError", "SchemaError"};
static_assert(kErrorCodeNames.size() == std::to_underlying(ErrorCode::SchemaError) + 1);

[[nodiscard]] constexpr std::string_view ErrorCodeName(ErrorCode code) noexcept {
  return kErrorCodeNames[std::to_underlying(code)];
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

  // Comparison is by code only — two errors with the same code are considered equal.
  [[nodiscard]] bool operator==(const Error& other) const noexcept { return code_ == other.code_; }
  [[nodiscard]] bool operator==(ErrorCode code) const noexcept { return code_ == code; }

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
};

// =============================================================================
// Status
// =============================================================================

using Status = std::expected<void, Error>;

// =============================================================================
// Result Type
// =============================================================================

/// @brief Result type using std::expected (C++23).
///
/// Result<T> holds either a value of type T on success, or an Error on failure.
///
/// @tparam T The success value type.
template <typename T>
using Result = std::expected<T, Error>;

// =============================================================================
// Helper Functions
// =============================================================================

[[nodiscard]] inline std::unexpected<Error> Err(
    ErrorCode code, std::string message,
    std::source_location loc = std::source_location::current()) {
  return std::unexpected<Error>{Error{code, std::move(message), loc}};
}

}  // namespace quest
