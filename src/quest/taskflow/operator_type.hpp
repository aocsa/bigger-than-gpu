#ifndef QUEST_TASKFLOW_OPERATOR_TYPE_HPP_
#define QUEST_TASKFLOW_OPERATOR_TYPE_HPP_

/// @file operator_type.hpp
/// @brief Defines operator types for the execution graph
/// @note Modern C++23 implementation focused on CPU/Disk memory tiers

#include <cstdint>
#include <string_view>

namespace quest::taskflow {

/// @brief Enumeration of all operator types in the execution graph
/// @details Each operator represents a specific computation unit that can be
///          scheduled and executed as part of a query plan
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

/// @brief Convert operator type to its string representation
/// @param type The operator type to convert
/// @return A string_view containing the operator name
[[nodiscard]] constexpr std::string_view to_string(OperatorType type) noexcept {
    switch (type) {
        case OperatorType::TableScan:           return "TableScan";
        case OperatorType::BindableTableScan:   return "BindableTableScan";
        case OperatorType::Generate:            return "Generate";
        case OperatorType::Project:             return "Project";
        case OperatorType::Filter:              return "Filter";
        case OperatorType::Union:               return "Union";
        case OperatorType::ComputeAggregate:    return "ComputeAggregate";
        case OperatorType::DistributeAggregate: return "DistributeAggregate";
        case OperatorType::MergeAggregate:      return "MergeAggregate";
        case OperatorType::SortAndSample:       return "SortAndSample";
        case OperatorType::Partition:           return "Partition";
        case OperatorType::PartitionSingleNode: return "PartitionSingleNode";
        case OperatorType::MergeStream:         return "MergeStream";
        case OperatorType::PartwiseJoin:        return "PartwiseJoin";
        case OperatorType::JoinPartition:       return "JoinPartition";
        case OperatorType::Limit:               return "Limit";
        case OperatorType::Output:              return "Output";
        case OperatorType::Print:               return "Print";
        case OperatorType::Unknown:             return "Unknown";
    }
    return "Unknown";
}

/// @brief Check if operator is a data source (has no required inputs)
[[nodiscard]] constexpr bool is_source_operator(OperatorType type) noexcept {
    return type == OperatorType::TableScan ||
           type == OperatorType::BindableTableScan ||
           type == OperatorType::Generate;
}

/// @brief Check if operator is a terminal (produces final output)
[[nodiscard]] constexpr bool is_terminal_operator(OperatorType type) noexcept {
    return type == OperatorType::Output ||
           type == OperatorType::Print;
}

/// @brief Check if operator requires multiple inputs (e.g., joins)
[[nodiscard]] constexpr bool is_multi_input_operator(OperatorType type) noexcept {
    return type == OperatorType::PartwiseJoin ||
           type == OperatorType::JoinPartition ||
           type == OperatorType::Union ||
           type == OperatorType::MergeStream;
}

/// @brief Check if operator is blocking (a pipeline breaker)
/// @details A blocking operator must consume (part of) its input before it can
///          produce output — aggregates, sorts, joins, partitions, unions.
///          In pipeline execution it becomes the sink of the producer pipeline
///          and the source of the consumer pipeline. Streaming operators
///          (Filter, Project, Limit) fuse into their pipeline instead.
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

#endif  // QUEST_TASKFLOW_OPERATOR_TYPE_HPP_
