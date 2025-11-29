#pragma once

/// @file result.hpp
/// @brief Result and Status types for error handling using C++23 std::expected.
///
/// This module provides a unified error handling approach for the Quest library
/// using the modern C++23 std::expected type. It avoids exceptions for expected
/// error conditions while still providing detailed error information.
///
/// Usage example:
/// @code
///   Result<int> parse_number(const std::string& str) {
///     try {
///       return std::stoi(str);
///     } catch (...) {
///       return std::unexpected(Status::Invalid("Failed to parse number"));
///     }
///   }
///
///   auto result = parse_number("42");
///   if (result.has_value()) {
///     std::print("Value: {}\n", result.value());
///   } else {
///     std::print("Error: {}\n", result.error().message());
///   }
/// @endcode

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

/// @brief Status codes for operation results.
///
/// These codes categorize errors to enable programmatic error handling
/// while still providing detailed error messages via Status::message().
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

// Modern Error Class
// Uses source_location to automatically capture where the error occurred.
struct Error {
  ErrorCode code_;
  std::string message_;
  std::source_location loc_;

  // ===========================================================================
  // Constructors
  // ===========================================================================

  /// @brief Primary constructor with code, message, and source location.
  Error(ErrorCode code, std::string message,
        std::source_location location = std::source_location::current())
      : code_(code), message_(std::move(message)), loc_(location) {}

  /// @brief Constructor with code and C-string message.
  Error(ErrorCode code, const char* message,
        std::source_location location = std::source_location::current())
      : code_(code), message_(message != nullptr ? message : ""), loc_(location) {}

  /// @brief Constructor with code and string_view message.
  Error(ErrorCode code, std::string_view message,
        std::source_location location = std::source_location::current())
      : code_(code), message_(message), loc_(location) {}

  // ===========================================================================
  // Copy and Move Constructors/Assignment
  // ===========================================================================

  /// @brief Copy constructor.
  Error(const Error& other) = default;

  /// @brief Move constructor.
  Error(Error&& other) noexcept = default;

  /// @brief Copy assignment operator.
  Error& operator=(const Error& other) = default;

  /// @brief Move assignment operator.
  Error& operator=(Error&& other) noexcept = default;

  /// @brief Destructor.
  ~Error() = default;

  // ===========================================================================
  // Accessors
  // ===========================================================================

  /// @brief Get the error code.
  [[nodiscard]] ErrorCode code() const noexcept { return code_; }

  /// @brief Get the error message.
  [[nodiscard]] const std::string& message() const noexcept { return message_; }

  /// @brief Get the source location where the error was created.
  [[nodiscard]] const std::source_location& location() const noexcept { return loc_; }

  // ===========================================================================
  // Formatting
  // ===========================================================================

  /// @brief Format error as human-readable string with location info.
  [[nodiscard]] std::string ToString() const {
    return std::format("[{}:{}] Error {}: {}", loc_.file_name(), loc_.line(),
                       static_cast<int>(code_), message_);
  }

  /// @brief Support for std::ostream.
  friend std::ostream& operator<<(std::ostream& out, const Error& error) {
    return out << error.ToString();
  }

  // ===========================================================================
  // Conversions
  // ===========================================================================

  /// @brief Conversion to C-string for exception compatibility.
  [[nodiscard]] explicit operator const char*() const { return message_.c_str(); }

  [[nodiscard]] explicit operator char*() { return (char*)message_.data(); }

  // ===========================================================================
  // Comparison Operators
  // ===========================================================================

  /// @brief Equality comparison (compares code only).
  [[nodiscard]] bool operator==(const Error& other) const noexcept {
    return code_ == other.code_;
  }

  /// @brief Inequality comparison.
  [[nodiscard]] bool operator!=(const Error& other) const noexcept {
    return code_ != other.code_;
  }

  /// @brief Compare with ErrorCode directly.
  [[nodiscard]] bool operator==(ErrorCode code) const noexcept {
    return code_ == code;
  }

  [[nodiscard]] bool operator!=(ErrorCode code) const noexcept {
    return code_ != code;
  }
};

// =============================================================================
// Status Class
// =============================================================================

/// @brief Status class representing success or failure with optional message.
///
/// Status carries both an error code and a descriptive message. Use the
/// static factory methods to create Status instances:
/// @code
///   Status::OK()                      // Success
///   Status::Invalid("bad input")      // Invalid argument
///   Status::IOError("file not found") // I/O error
/// @endcode
class Status : public std::expected<void, Error> {
 public:
  using std::expected<void, Error>::expected;  // Inherit constructors

  // ===========================================================================
  // Factory Methods
  // ===========================================================================

  /// @brief Create a successful status.
  [[nodiscard]] static Status OK() noexcept {
    return {};  // Default-constructed Status is successful (has_value() == true)
  }

  /// @brief Create an invalid argument error.
  [[nodiscard]] static std::unexpected<Error>  Invalid(std::string message) {
    return std::unexpected<Error>{Error(ErrorCode::Invalid, std::move(message))};
  }

  /// @brief Create an I/O error.
  [[nodiscard]] static std::unexpected<Error>  IOError(std::string message) {
    return std::unexpected<Error>{Error(ErrorCode::IOError, std::move(message))};
  }

  /// @brief Create an out of memory error.
  [[nodiscard]] static std::unexpected<Error>  OutOfMemory(std::string message) {
    return std::unexpected<Error>{Error(ErrorCode::OutOfMemory, std::move(message))};
  }

  /// @brief Create a not found error.
  [[nodiscard]] static std::unexpected<Error>  NotFound(std::string message) {
    return std::unexpected<Error>{Error(ErrorCode::NotFound, std::move(message))};
  }

  /// @brief Create an already exists error.
  [[nodiscard]] static std::unexpected<Error>  AlreadyExists(std::string message) {
    return std::unexpected<Error>{Error(ErrorCode::AlreadyExists, std::move(message))};
  }

  /// @brief Create a cancelled error.
  [[nodiscard]] static std::unexpected<Error>  Cancelled(std::string message) {
    return std::unexpected<Error>{Error(ErrorCode::Cancelled, std::move(message))};
  }

  /// @brief Create a Parquet format error.
  [[nodiscard]] static std::unexpected<Error>  ParquetFormatError(std::string message) {
    return std::unexpected<Error>{Error(ErrorCode::ParquetFormatError, std::move(message))};
  }

  /// @brief Create an unsupported encoding error.
  [[nodiscard]] static std::unexpected<Error>  UnsupportedEncoding(std::string message) {
    return std::unexpected<Error>{Error(ErrorCode::UnsupportedEncoding, std::move(message))};
  }

  /// @brief Create a Thrift decode error.
  [[nodiscard]] static std::unexpected<Error>  ThriftDecodeError(std::string message) {
    return std::unexpected<Error>{Error(ErrorCode::ThriftDecodeError, std::move(message))};
  }

  /// @brief Create a page decode error.
  [[nodiscard]] static std::unexpected<Error>  PageDecodeError(std::string message) {
    return std::unexpected<Error>{Error(ErrorCode::PageDecodeError, std::move(message))};
  }

  /// @brief Create a column decode error.
  [[nodiscard]] static std::unexpected<Error>  ColumnDecodeError(std::string message) {
    return std::unexpected<Error>{Error(ErrorCode::ColumnDecodeError, std::move(message))};
  }

  /// @brief Create a type error.
  [[nodiscard]] static std::unexpected<Error>  TypeError(std::string message) {
    return std::unexpected<Error>{Error(ErrorCode::TypeError, std::move(message))};
  }

  /// @brief Create an out of range error.
  [[nodiscard]] static std::unexpected<Error>  OutOfRange(std::string message) {
    return std::unexpected<Error>{Error(ErrorCode::OutOfRange, std::move(message))};
  }

  /// @brief Create an end of data status (for iteration).
  [[nodiscard]] static std::unexpected<Error>  EndOfData() {
    return std::unexpected<Error>{Error(ErrorCode::EndOfData, {})};
  }

  /// @brief Create a logical plan error.
  [[nodiscard]] static std::unexpected<Error>  LogicalPlanError(std::string message) {
    return std::unexpected<Error>{Error(ErrorCode::LogicalPlanError, std::move(message))};
  }

  /// @brief Create a physical plan error.
  [[nodiscard]] static std::unexpected<Error>  PhysicalPlanError(std::string message) {
    return std::unexpected<Error>{Error(ErrorCode::PhysicalPlanError, std::move(message))};
  }

  /// @brief Create an expression evaluation error.
  [[nodiscard]] static std::unexpected<Error>  ExpressionError(std::string message) {
    return std::unexpected<Error>{Error(ErrorCode::ExpressionError, std::move(message))};
  }

  /// @brief Create a schema error.
  [[nodiscard]] static std::unexpected<Error>  SchemaError(std::string message) {
    return std::unexpected<Error>{Error(ErrorCode::SchemaError, std::move(message))};
  }

  // ===========================================================================
  // Query Methods
  // ===========================================================================

  /// @brief Check if status represents success.
  [[nodiscard]] bool ok() const noexcept { return this->has_value(); }

  /// @brief Explicit bool conversion (true if OK).
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  /// @brief Get the status code.
  [[nodiscard]] ErrorCode code() const noexcept {
    return this->has_value() ? ErrorCode::None : this->error().code_;
  }

  /// @brief Get the error message.
  [[nodiscard]] std::string_view message() const noexcept {
    return this->has_value() ? std::string_view{} : this->error().message_;
  }

  /// @brief Check if this is an end-of-data status.
  [[nodiscard]] bool is_end_of_data() const noexcept {
    return !this->has_value() && this->error().code_ == ErrorCode::EndOfData;
  }

  // ===========================================================================
  // Formatting
  // ===========================================================================

  /// @brief Format status as human-readable string.
  [[nodiscard]] std::string ToString() const {
    if (ok()) {
      return "OK";
    }
    return error().ToString();
  }

 private:

  [[nodiscard]] static std::string_view code_to_string(ErrorCode code) noexcept {
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

/// @brief Create a successful result.
///
/// @code
///   return Ok(42);
/// @endcode
template <typename T>
[[nodiscard]] Result<T> Ok(T value) {
  return Result<T>{std::in_place, std::move(value)};
}

/// @brief Create an error result.
///
/// @code
///   return Err(Error::Invalid, "bad input");
/// @endcode
[[nodiscard]] inline std::unexpected<Error> Err(const ErrorCode& code, std::string message) {
  return std::unexpected<Error>{Error{code, std::move(message)}};
}


/// @brief Create an error result.
///
/// @code
///   return Err(Error(ErrorCode::Invalid, "bad input"));
/// @endcode
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

