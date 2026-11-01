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

struct Error;

/// @brief The type of `Error::Invalid`, `Error::IOError`, ...: a call makes an error of code
/// @p C as a `std::unexpected<Error>`.
template <ErrorCode C>
struct ErrorFactory {
  /// @param message The error message.
  /// @param loc The caller's source location.
  [[nodiscard]] std::unexpected<Error> operator()(
      std::string message, std::source_location loc = std::source_location::current()) const;
};

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

  static constexpr ErrorFactory<ErrorCode::Invalid> Invalid{};
  static constexpr ErrorFactory<ErrorCode::IOError> IOError{};
  static constexpr ErrorFactory<ErrorCode::OutOfMemory> OutOfMemory{};
  static constexpr ErrorFactory<ErrorCode::NotFound> NotFound{};
  static constexpr ErrorFactory<ErrorCode::AlreadyExists> AlreadyExists{};
  static constexpr ErrorFactory<ErrorCode::Cancelled> Cancelled{};
  static constexpr ErrorFactory<ErrorCode::ParquetFormatError> ParquetFormatError{};
  static constexpr ErrorFactory<ErrorCode::UnsupportedEncoding> UnsupportedEncoding{};
  static constexpr ErrorFactory<ErrorCode::ThriftDecodeError> ThriftDecodeError{};
  static constexpr ErrorFactory<ErrorCode::PageDecodeError> PageDecodeError{};
  static constexpr ErrorFactory<ErrorCode::ColumnDecodeError> ColumnDecodeError{};
  static constexpr ErrorFactory<ErrorCode::TypeError> TypeError{};
  static constexpr ErrorFactory<ErrorCode::OutOfRange> OutOfRange{};
  static constexpr ErrorFactory<ErrorCode::LogicalPlanError> LogicalPlanError{};
  static constexpr ErrorFactory<ErrorCode::PhysicalPlanError> PhysicalPlanError{};
  static constexpr ErrorFactory<ErrorCode::ExpressionError> ExpressionError{};
  static constexpr ErrorFactory<ErrorCode::SchemaError> SchemaError{};
};

// Defined after Error: the body constructs an Error, which must be complete here.
template <ErrorCode C>
std::unexpected<Error> ErrorFactory<C>::operator()(std::string message,
                                                   std::source_location loc) const {
  return std::unexpected<Error>{Error(C, std::move(message), loc)};
}

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
