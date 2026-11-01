#pragma once

/// @file operator_type.hpp
/// @brief Operator type enum and classification predicates for the execution graph.

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

namespace quest::taskflow {

enum class OperatorType : std::uint8_t {
    // Data Source Operators
    TableScan,
    BindableTableScan,
    Generate,

    // Transformation Operators
    Project,
    Filter,

    // Set Operators
    Union,

    // Aggregation Operators
    ComputeAggregate,
    DistributeAggregate,
    MergeAggregate,

    // Sort Operators
    SortAndSample,
    Partition,
    PartitionSingleNode,
    MergeStream,

    // Join Operators
    PartwiseJoin,
    JoinPartition,

    // Limit/Output Operators
    Limit,
    Output,
    Print,

    // Sentinel
    Unknown
};

inline constexpr std::array<std::string_view, 19> kOperatorTypeNames = {
    "TableScan", "BindableTableScan", "Generate", "Project", "Filter", "Union", "ComputeAggregate",
    "DistributeAggregate", "MergeAggregate", "SortAndSample", "Partition", "PartitionSingleNode",
    "MergeStream", "PartwiseJoin", "JoinPartition", "Limit", "Output", "Print", "Unknown"};
static_assert(kOperatorTypeNames.size() == std::to_underlying(OperatorType::Unknown) + 1);

[[nodiscard]] constexpr std::string_view to_string(OperatorType type) noexcept {
    return kOperatorTypeNames[std::to_underlying(type)];
}

/// @brief Check if operator is a data source (has no required inputs)
[[nodiscard]] constexpr bool is_source_operator(OperatorType type) noexcept {
    return type == OperatorType::TableScan ||
           type == OperatorType::BindableTableScan ||
           type == OperatorType::Generate;
}

/// @brief A blocking operator must consume (part of) its input before producing output.
///
/// Becomes the sink of the producer pipeline and the source of the consumer pipeline.
/// Streaming operators (Filter, Project, Limit) fuse into their pipeline instead.
[[nodiscard]] constexpr bool is_blocking_operator(OperatorType type) noexcept {
    return type == OperatorType::ComputeAggregate ||
           type == OperatorType::DistributeAggregate ||
           type == OperatorType::MergeAggregate ||
           type == OperatorType::SortAndSample ||
           type == OperatorType::Partition ||
           type == OperatorType::PartitionSingleNode ||
           type == OperatorType::MergeStream ||
           type == OperatorType::PartwiseJoin ||
           type == OperatorType::JoinPartition ||
           type == OperatorType::Union;
}

}  // namespace quest::taskflow
