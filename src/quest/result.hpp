#pragma once

/// @file result.hpp
/// @brief Result and Status types for error handling using C++23 std::expected

#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace quest {

/// @brief Status codes for operation results
enum class StatusCode {
    OK,
    Invalid,
    IOError,
    OutOfMemory,
    NotFound,
    AlreadyExists,
    Cancelled,
    Unknown
};

/// @brief Status class representing success or failure with optional message
class Status {
public:
    /// @brief Create a successful status
    [[nodiscard]] static Status OK() noexcept {
        return Status{StatusCode::OK, {}};
    }

    /// @brief Create an invalid argument error
    [[nodiscard]] static Status Invalid(std::string message) {
        return Status{StatusCode::Invalid, std::move(message)};
    }

    /// @brief Create an I/O error
    [[nodiscard]] static Status IOError(std::string message) {
        return Status{StatusCode::IOError, std::move(message)};
    }

    /// @brief Create an out of memory error
    [[nodiscard]] static Status OutOfMemory(std::string message) {
        return Status{StatusCode::OutOfMemory, std::move(message)};
    }

    /// @brief Create a not found error
    [[nodiscard]] static Status NotFound(std::string message) {
        return Status{StatusCode::NotFound, std::move(message)};
    }

    /// @brief Create an already exists error
    [[nodiscard]] static Status AlreadyExists(std::string message) {
        return Status{StatusCode::AlreadyExists, std::move(message)};
    }

    /// @brief Create a cancelled error
    [[nodiscard]] static Status Cancelled(std::string message) {
        return Status{StatusCode::Cancelled, std::move(message)};
    }

    /// @brief Check if status represents success
    [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::OK; }

    /// @brief Explicit bool conversion (true if OK)
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

    /// @brief Get the status code
    [[nodiscard]] StatusCode code() const noexcept { return code_; }

    /// @brief Get the error message
    [[nodiscard]] std::string_view message() const noexcept { return message_; }

    /// @brief Format status as string
    [[nodiscard]] std::string ToString() const {
        if (ok()) {
            return "OK";
        }
        return std::format("{}: {}", code_to_string(code_), message_);
    }

private:
    Status(StatusCode code, std::string message)
        : code_(code), message_(std::move(message)) {}

    [[nodiscard]] static std::string_view code_to_string(StatusCode code) noexcept {
        switch (code) {
            case StatusCode::OK:            return "OK";
            case StatusCode::Invalid:       return "Invalid";
            case StatusCode::IOError:       return "IOError";
            case StatusCode::OutOfMemory:   return "OutOfMemory";
            case StatusCode::NotFound:      return "NotFound";
            case StatusCode::AlreadyExists: return "AlreadyExists";
            case StatusCode::Cancelled:     return "Cancelled";
            case StatusCode::Unknown:       return "Unknown";
        }
        return "Unknown";
    }

    StatusCode code_;
    std::string message_;
};

/// @brief Result type using std::expected (C++23)
/// @tparam T The success value type
template <typename T>
using Result = std::expected<T, Status>;

/// @brief Create a successful result
template <typename T>
[[nodiscard]] Result<T> Ok(T value) {
    return Result<T>{std::in_place, std::move(value)};
}

/// @brief Create an error result
template <typename T>
[[nodiscard]] Result<T> Err(Status status) {
    return std::unexpected{std::move(status)};
}

}  // namespace quest
