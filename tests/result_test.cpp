// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file result_test.cpp
/// @brief Unit tests for Result and Status error handling.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "quest/result.hpp"
#include "quest/tests/utils.hpp"

using namespace quest;

// =============================================================================
// Status Tests
// =============================================================================

TEST(StatusTest, OK) {
    Status status = Status::OK();
    EXPECT_TRUE(status.has_value());
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(status.code(), ErrorCode::None);
    EXPECT_TRUE(status.message().empty());
}

TEST(StatusTest, Invalid) {
    Status status = Status::Invalid("Test error");
    EXPECT_FALSE(status.has_value());
    EXPECT_FALSE(status);

    const auto& error = status.error();
    EXPECT_EQ(error.code(), ErrorCode::Invalid);
    EXPECT_EQ(error.message(), "Test error");
}

TEST(StatusTest, IOError) {
    Status status = Status::IOError("File not found");
    EXPECT_FALSE(status.has_value());

    const auto& error = status.error();
    EXPECT_EQ(error.code(), ErrorCode::IOError);
    EXPECT_EQ(error.message(), "File not found");
}

TEST(StatusTest, OutOfMemory) {
    Status status = Status::OutOfMemory("Allocation failed");
    EXPECT_FALSE(status.has_value());

    const auto& error = status.error();
    EXPECT_EQ(error.code(), ErrorCode::OutOfMemory);
    EXPECT_EQ(error.message(), "Allocation failed");
}

TEST(StatusTest, NotFound) {
    Status status = Status::NotFound("Resource missing");
    EXPECT_FALSE(status.has_value());

    const auto& error = status.error();
    EXPECT_EQ(error.code(), ErrorCode::NotFound);
    EXPECT_EQ(error.message(), "Resource missing");
}

TEST(StatusTest, OkMethod) {
    Status error_status = Status::Invalid("Error");
    EXPECT_FALSE(error_status.ok());

    // A default-constructed Status should be ok
    Status ok_status;
    EXPECT_TRUE(ok_status.ok());
}

// =============================================================================
// Error Tests
// =============================================================================

TEST(ErrorTest, Construction) {
    Error error(ErrorCode::Invalid, "Test message");
    EXPECT_EQ(error.code(), ErrorCode::Invalid);
    EXPECT_EQ(error.message(), "Test message");
}

TEST(ErrorTest, ConstructionWithCString) {
    Error error(ErrorCode::IOError, "C-string message");
    EXPECT_EQ(error.code(), ErrorCode::IOError);
    EXPECT_EQ(error.message(), "C-string message");
}

TEST(ErrorTest, ConstructionWithEmptyMessage) {
    Error error(ErrorCode::OutOfMemory, "");
    EXPECT_EQ(error.code(), ErrorCode::OutOfMemory);
    EXPECT_TRUE(error.message().empty());
}

TEST(ErrorTest, SourceLocation) {
    Error error(ErrorCode::Invalid, "Test");
    const auto& loc = error.location();
    EXPECT_GT(loc.line(), 0U);
    EXPECT_NE(std::string_view(loc.file_name()).find("result_test.cpp"),
              std::string_view::npos);
}

TEST(ErrorTest, FactoryRecordsCallerLocation) {
    Status status = Status::Invalid("bad");
    EXPECT_NE(std::string_view(status.error().location().file_name()).find("result_test.cpp"),
              std::string_view::npos);
}

TEST(ErrorTest, ToString) {
    Error error(ErrorCode::IOError, "File not found");
    std::string str = error.ToString();

    EXPECT_TRUE(str.find("File not found") != std::string::npos);
    EXPECT_TRUE(str.find("IOError") != std::string::npos);
}

TEST(ErrorTest, ComparisonWithErrorCode) {
    Error error(ErrorCode::Invalid, "Error");
    EXPECT_TRUE(error == ErrorCode::Invalid);
    EXPECT_FALSE(error == ErrorCode::IOError);
    EXPECT_TRUE(error != ErrorCode::IOError);
}

// =============================================================================
// Result Tests
// =============================================================================

TEST(ResultTest, WithValue) {
    Result<int> result = 42;
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);
}

TEST(ResultTest, WithError) {
    Result<int> result = Status::Invalid("Parse error");
    EXPECT_FALSE(result.has_value());

    const auto& error = result.error();
    EXPECT_EQ(error.message(), "Parse error");
    EXPECT_EQ(error.code(), ErrorCode::Invalid);
}

TEST(ResultTest, ValueOr) {
    Result<int> success = 42;
    EXPECT_EQ(success.value_or(0), 42);

    Result<int> failure = Status::Invalid("Error");
    EXPECT_EQ(failure.value_or(0), 0);
}

// =============================================================================
// Monadic Operations Tests
// =============================================================================

TEST(ResultTest, MonadicAndThen) {
    auto parse_positive = [](const std::string& s) -> Result<int> {
        int val = std::stoi(s);
        if (val <= 0) {
            return Status::Invalid("Value must be positive");
        }
        return val;
    };

    auto double_value = [](int x) -> Result<int> {
        return x * 2;
    };

    Result<int> result = parse_positive("5").and_then(double_value);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 10);

    Result<int> result_error = parse_positive("-5").and_then(double_value);
    EXPECT_FALSE(result_error.has_value());
}

TEST(ResultTest, MonadicTransform) {
    Result<int> result = Result<int>(5).transform([](int x) { return x * 3; });
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 15);

    Result<int> error_result = Status::Invalid("Error");
    auto transformed = error_result.transform([](int x) { return x * 3; });
    EXPECT_FALSE(transformed.has_value());
}

TEST(ResultTest, MonadicOrElse) {
    Result<int> error_result = Status::Invalid("Error");

    auto recovered = error_result.or_else([](const Error&) -> Result<int> {
        return 42;  // Default value
    });

    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(*recovered, 42);
}

// =============================================================================
// Ok Helper Function Tests
// =============================================================================

TEST(HelperTest, OkFunction) {
    Result<int> result = Ok(42);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 42);
}

TEST(HelperTest, OkFunctionWithString) {
    Result<std::string> result = Ok(std::string("hello"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "hello");
}

// =============================================================================
// ErrorCode Tests
// =============================================================================

TEST(ErrorCodeTest, AllCodes) {
    EXPECT_NE(ErrorCode::None, ErrorCode::Invalid);
    EXPECT_NE(ErrorCode::Invalid, ErrorCode::IOError);
    EXPECT_NE(ErrorCode::IOError, ErrorCode::OutOfMemory);
    EXPECT_NE(ErrorCode::OutOfMemory, ErrorCode::NotFound);

    EXPECT_EQ(static_cast<int>(ErrorCode::None), 0);
}

// =============================================================================
// Additional Status Factory Method Tests
// =============================================================================

TEST(StatusTest, AlreadyExists) {
    Status status = Status::AlreadyExists("Resource exists");
    EXPECT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), ErrorCode::AlreadyExists);
    EXPECT_EQ(status.error().message(), "Resource exists");
}

TEST(StatusTest, Cancelled) {
    Status status = Status::Cancelled("Operation cancelled");
    EXPECT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), ErrorCode::Cancelled);
    EXPECT_EQ(status.error().message(), "Operation cancelled");
}

TEST(StatusTest, ParquetFormatError) {
    Status status = Status::ParquetFormatError("Bad parquet");
    EXPECT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), ErrorCode::ParquetFormatError);
}

TEST(StatusTest, UnsupportedEncoding) {
    Status status = Status::UnsupportedEncoding("Unknown encoding");
    EXPECT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), ErrorCode::UnsupportedEncoding);
}

TEST(StatusTest, ThriftDecodeError) {
    Status status = Status::ThriftDecodeError("Thrift error");
    EXPECT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), ErrorCode::ThriftDecodeError);
}

TEST(StatusTest, PageDecodeError) {
    Status status = Status::PageDecodeError("Page error");
    EXPECT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), ErrorCode::PageDecodeError);
}

TEST(StatusTest, ColumnDecodeError) {
    Status status = Status::ColumnDecodeError("Column error");
    EXPECT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), ErrorCode::ColumnDecodeError);
}

TEST(StatusTest, TypeError) {
    Status status = Status::TypeError("Type mismatch");
    EXPECT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), ErrorCode::TypeError);
}

TEST(StatusTest, OutOfRange) {
    Status status = Status::OutOfRange("Index out of range");
    EXPECT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), ErrorCode::OutOfRange);
}

TEST(StatusTest, EndOfData) {
    Status status = Status::EndOfData();
    EXPECT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), ErrorCode::EndOfData);
    EXPECT_TRUE(status.is_end_of_data());
}

TEST(StatusTest, LogicalPlanError) {
    Status status = Status::LogicalPlanError("Plan error");
    EXPECT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), ErrorCode::LogicalPlanError);
}

TEST(StatusTest, PhysicalPlanError) {
    Status status = Status::PhysicalPlanError("Physical plan error");
    EXPECT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), ErrorCode::PhysicalPlanError);
}

TEST(StatusTest, ExpressionError) {
    Status status = Status::ExpressionError("Expr error");
    EXPECT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), ErrorCode::ExpressionError);
}

TEST(StatusTest, SchemaError) {
    Status status = Status::SchemaError("Schema mismatch");
    EXPECT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), ErrorCode::SchemaError);
}

// =============================================================================
// Status Method Tests
// =============================================================================

TEST(StatusTest, CodeMethod) {
    Status ok_status = Status::OK();
    EXPECT_EQ(ok_status.code(), ErrorCode::None);

    Status error_status = Status::Invalid("error");
    EXPECT_EQ(error_status.code(), ErrorCode::Invalid);
}

TEST(StatusTest, MessageMethod) {
    Status ok_status = Status::OK();
    EXPECT_TRUE(ok_status.message().empty());

    Status error_status = Status::Invalid("test message");
    EXPECT_EQ(error_status.message(), "test message");
}

TEST(StatusTest, IsEndOfData) {
    Status ok_status = Status::OK();
    EXPECT_FALSE(ok_status.is_end_of_data());

    Status invalid_status = Status::Invalid("error");
    EXPECT_FALSE(invalid_status.is_end_of_data());

    Status end_status = Status::EndOfData();
    EXPECT_TRUE(end_status.is_end_of_data());
}

TEST(StatusTest, ToStringMethod) {
    Status ok_status = Status::OK();
    EXPECT_EQ(ok_status.ToString(), "OK");

    Status error_status = Status::Invalid("test error");
    std::string str = error_status.ToString();
    EXPECT_TRUE(str.find("test error") != std::string::npos);
}

// =============================================================================
// Error Edge Case Tests
// =============================================================================

TEST(ErrorTest, ConstructionWithNullptr) {
    Error error(ErrorCode::Invalid, nullptr);
    EXPECT_EQ(error.code(), ErrorCode::Invalid);
    EXPECT_TRUE(error.message().empty());
}

TEST(ErrorTest, CopyConstructor) {
    Error original(ErrorCode::IOError, "original message");
    Error copy(original);

    EXPECT_EQ(copy.code(), ErrorCode::IOError);
    EXPECT_EQ(copy.message(), "original message");
}

TEST(ErrorTest, MoveConstructor) {
    Error original(ErrorCode::IOError, "original message");
    Error moved(std::move(original));

    EXPECT_EQ(moved.code(), ErrorCode::IOError);
    EXPECT_EQ(moved.message(), "original message");
}

TEST(ErrorTest, CopyAssignment) {
    Error original(ErrorCode::IOError, "original");
    Error other(ErrorCode::Invalid, "other");

    other = original;
    EXPECT_EQ(other.code(), ErrorCode::IOError);
    EXPECT_EQ(other.message(), "original");
}

TEST(ErrorTest, MoveAssignment) {
    Error original(ErrorCode::IOError, "original");
    Error other(ErrorCode::Invalid, "other");

    other = std::move(original);
    EXPECT_EQ(other.code(), ErrorCode::IOError);
    EXPECT_EQ(other.message(), "original");
}

TEST(ErrorTest, EqualityComparison) {
    Error error1(ErrorCode::Invalid, "message 1");
    Error error2(ErrorCode::Invalid, "message 2");
    Error error3(ErrorCode::IOError, "message 1");

    // Equality compares code only
    EXPECT_TRUE(error1 == error2);
    EXPECT_FALSE(error1 == error3);
    EXPECT_TRUE(error1 != error3);
}

TEST(ErrorTest, ExplicitCharConversion) {
    Error error(ErrorCode::Invalid, "test message");
    const char* msg = static_cast<const char*>(error);
    EXPECT_STREQ(msg, "test message");
}

TEST(ErrorTest, LongErrorMessage) {
    std::string long_msg(1000, 'x');
    Error error(ErrorCode::Invalid, long_msg);
    EXPECT_EQ(error.message().length(), 1000U);
}

TEST(ErrorTest, UnicodeMessage) {
    Error error(ErrorCode::Invalid, "Error: 日本語 🎉");
    EXPECT_TRUE(error.message().find("日本語") != std::string::npos);
}

// =============================================================================
// Result Edge Case Tests
// =============================================================================

TEST(ResultTest, WithMoveOnlyType) {
    Result<std::unique_ptr<int>> result = std::make_unique<int>(42);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(**result, 42);
}

TEST(ResultTest, ChainedOperations) {
    auto add_one = [](int x) -> Result<int> { return x + 1; };
    auto multiply_two = [](int x) { return x * 2; };

    Result<int> result = Result<int>(5)
        .and_then(add_one)
        .transform(multiply_two);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 12);  // (5 + 1) * 2
}

TEST(ResultTest, TransformError) {
    Result<int> error_result = Status::Invalid("original");

    auto transformed = error_result.transform_error([](const Error& e) {
        return Error(e.code(), "transformed: " + std::string(e.message()));
    });

    EXPECT_FALSE(transformed.has_value());
    EXPECT_TRUE(transformed.error().message().find("transformed") != std::string::npos);
}

TEST(ResultTest, ErrorPropagationChain) {
    auto step1 = []() -> Result<int> {
        return Status::Invalid("step1 failed");
    };

    auto step2 = [](int x) -> Result<int> {
        return x * 2;
    };

    auto step3 = [](int x) -> Result<int> {
        return x + 1;
    };

    Result<int> result = step1().and_then(step2).and_then(step3);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message(), "step1 failed");
}

// =============================================================================
// Err Helper Function Tests
// =============================================================================

TEST(HelperTest, ErrFunction) {
    Result<int> result = Err(ErrorCode::Invalid, "error message");

    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Invalid);
    EXPECT_EQ(result.error().message(), "error message");
}

Result<int> ParseInt(const std::string& input) {
    if (input.empty()) {
        return Status::Invalid("Empty string");
    }
    try {
        return std::stoi(input);
    } catch (...) {
        return Status::Invalid("Parse error");
    }
}

// =============================================================================
// Test Utils Macro Tests (from utils.hpp)
// =============================================================================

TEST(UtilsMacroTest, AssertOkWithOkStatus) {
    Status ok_status = Status::OK();
    ASSERT_OK(ok_status);
}

TEST(UtilsMacroTest, AssertOkWithOkResult) {
    Result<int> result = 42;
    ASSERT_OK(result);
}

TEST(UtilsMacroTest, AssertOkAndAssignSuccess) {
    Result<int> result = 42;
    ASSERT_OK_AND_ASSIGN(auto value, result);
    EXPECT_EQ(value, 42);
}

TEST(UtilsMacroTest, AssertOkAndAssignWithString) {
    Result<std::string> result = std::string("hello");
    ASSERT_OK_AND_ASSIGN(auto value, result);
    EXPECT_EQ(value, "hello");
}

TEST(UtilsMacroTest, AssertOkAndAssignFromFunction) {
    ASSERT_OK_AND_ASSIGN(auto value, ParseInt("123"));
    EXPECT_EQ(value, 123);
}
