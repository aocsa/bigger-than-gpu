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

namespace quest {

// =============================================================================
// Error Codes
// =============================================================================

/// @brief Status codes for operation results.
///
/// These codes categorize errors to enable programmatic error handling
/// while still providing detailed error messages via Status::message().
enum class ErrorCode {
  // Operation completed successfully.
  OK,

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
  EndOfData
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
class Status {
 public:
  // ===========================================================================
  // Factory Methods
  // ===========================================================================

  /// @brief Create a successful status.
  [[nodiscard]] static Status OK() noexcept { return Status{ErrorCode::OK, {}}; }

  /// @brief Create an invalid argument error.
  [[nodiscard]] static Status Invalid(std::string message) {
    return Status{ErrorCode::Invalid, std::move(message)};
  }

  /// @brief Create an I/O error.
  [[nodiscard]] static Status IOError(std::string message) {
    return Status{ErrorCode::IOError, std::move(message)};
  }

  /// @brief Create an out of memory error.
  [[nodiscard]] static Status OutOfMemory(std::string message) {
    return Status{ErrorCode::OutOfMemory, std::move(message)};
  }

  /// @brief Create a not found error.
  [[nodiscard]] static Status NotFound(std::string message) {
    return Status{ErrorCode::NotFound, std::move(message)};
  }

  /// @brief Create an already exists error.
  [[nodiscard]] static Status AlreadyExists(std::string message) {
    return Status{ErrorCode::AlreadyExists, std::move(message)};
  }

  /// @brief Create a cancelled error.
  [[nodiscard]] static Status Cancelled(std::string message) {
    return Status{ErrorCode::Cancelled, std::move(message)};
  }

  /// @brief Create a Parquet format error.
  [[nodiscard]] static Status ParquetFormatError(std::string message) {
    return Status{ErrorCode::ParquetFormatError, std::move(message)};
  }

  /// @brief Create an unsupported encoding error.
  [[nodiscard]] static Status UnsupportedEncoding(std::string message) {
    return Status{ErrorCode::UnsupportedEncoding, std::move(message)};
  }

  /// @brief Create a Thrift decode error.
  [[nodiscard]] static Status ThriftDecodeError(std::string message) {
    return Status{ErrorCode::ThriftDecodeError, std::move(message)};
  }

  /// @brief Create a page decode error.
  [[nodiscard]] static Status PageDecodeError(std::string message) {
    return Status{ErrorCode::PageDecodeError, std::move(message)};
  }

  /// @brief Create a column decode error.
  [[nodiscard]] static Status ColumnDecodeError(std::string message) {
    return Status{ErrorCode::ColumnDecodeError, std::move(message)};
  }

  /// @brief Create a type error.
  [[nodiscard]] static Status TypeError(std::string message) {
    return Status{ErrorCode::TypeError, std::move(message)};
  }

  /// @brief Create an out of range error.
  [[nodiscard]] static Status OutOfRange(std::string message) {
    return Status{ErrorCode::OutOfRange, std::move(message)};
  }

  /// @brief Create an end of data status (for iteration).
  [[nodiscard]] static Status EndOfData() {
    return Status{ErrorCode::EndOfData, {}};
  }

  // ===========================================================================
  // Query Methods
  // ===========================================================================

  /// @brief Check if status represents success.
  [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::OK; }

  /// @brief Explicit bool conversion (true if OK).
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  /// @brief Get the status code.
  [[nodiscard]] ErrorCode code() const noexcept { return code_; }

  /// @brief Get the error message.
  [[nodiscard]] std::string_view message() const noexcept { return message_; }

  /// @brief Check if this is an end-of-data status.
  [[nodiscard]] bool is_end_of_data() const noexcept {
    return code_ == ErrorCode::EndOfData;
  }

  // ===========================================================================
  // Formatting
  // ===========================================================================

  /// @brief Format status as human-readable string.
  [[nodiscard]] std::string ToString() const {
    if (ok()) {
      return "OK";
    }
    return std::format("{}: {}", code_to_string(code_), message_);
  }

 private:
  Status(ErrorCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  [[nodiscard]] static std::string_view code_to_string(ErrorCode code) noexcept {
    switch (code) {
      case ErrorCode::OK:
        return "OK";
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
    }
    return "Unknown";
  }

  ErrorCode code_;
  std::string message_;
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
using Result = std::expected<T, Status>;

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
///   return Err<int>(Status::Invalid("bad input"));
/// @endcode
template <typename T>
[[nodiscard]] Result<T> Err(Status status) {
  return std::unexpected{std::move(status)};
}

// =============================================================================
// Macros for Error Propagation
// =============================================================================

/// @brief Return early if the result is an error.
///
/// This macro simplifies error propagation in functions that return Result<T>.
/// If the expression evaluates to an error, the macro returns that error
/// immediately. Otherwise, it extracts the value.
///
/// @code
///   Result<int> parse_file(const std::string& path) {
///     QUEST_ASSIGN_OR_RETURN(auto file, open_file(path));
///     QUEST_ASSIGN_OR_RETURN(auto content, read_content(file));
///     return parse_content(content);
///   }
/// @endcode
#define QUEST_ASSIGN_OR_RETURN(var, expr)               \
  auto _result_##__LINE__ = (expr);                     \
  if (!_result_##__LINE__.has_value()) {                \
    return std::unexpected(_result_##__LINE__.error()); \
  }                                                     \
  var = std::move(_result_##__LINE__.value())

/// @brief Return early if the status is not OK.
///
/// @code
///   Status validate_and_process(const Data& data) {
///     QUEST_RETURN_IF_ERROR(validate(data));
///     return process(data);
///   }
/// @endcode
#define QUEST_RETURN_IF_ERROR(expr) \
  do {                              \
    auto _status = (expr);          \
    if (!_status.ok()) {            \
      return _status;               \
    }                               \
  } while (false)

}  // namespace quest
