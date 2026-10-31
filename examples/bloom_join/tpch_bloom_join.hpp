// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file tpch_bloom_join.hpp
/// @brief Educational batched inner join over non-null TPC-H column projections.

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "quest/execution/bloom_filter.hpp"

namespace quest::examples {

/// @brief Projection of TPC-H PART used by the build input.
struct Part {
  int64_t part_key;
  std::string brand;
};

/// @brief Projection of TPC-H LINEITEM; prices are integer cents.
struct Lineitem {
  int64_t order_key;
  int64_t part_key;
  int64_t extended_price_cents;
  bool operator==(const Lineitem&) const = default;
};

/// @brief Per-batch output and counters; each matching build row emits one lineitem.
struct ProbeResult {
  std::vector<Lineitem> rows;
  std::size_t bloom_rejected = 0;
  std::size_t exact_lookups = 0;
};

// =============================================================================
// Build batches -> finalize -> probe batches
// =============================================================================

/// @brief Implements SELECT l.* FROM lineitem l JOIN part p ON l.partkey=p.partkey
/// WHERE p.brand=selected_brand. This helper is not a QUEST pipeline Operator.
/// Inputs are borrowed only for the duration of a call. Build state owns its keys.
class TpchBloomJoin {
 public:
  /// @brief Construct a join with a validated, empty Bloom filter.
  /// @param selected_brand Build-side equality predicate.
  /// @param bit_count Power-of-two filter capacity in bits.
  /// @param hash_count Bloom probes per key.
  /// @return Join or a configuration error.
  [[nodiscard]] static Result<TpchBloomJoin> Create(std::string selected_brand,
                                                   std::size_t bit_count = 1024,
                                                   std::size_t hash_count = 7) {
    auto bloom = execution::BloomFilter::Create(bit_count, hash_count);
    if (!bloom) return std::unexpected(bloom.error());
    return TpchBloomJoin(std::move(selected_brand), std::move(*bloom));
  }

  /// @brief Consume one build batch, applying the brand predicate before insertion.
  /// @param batch Borrowed PART rows; may be empty.
  /// @return Success, or Invalid if build has already been finalized.
  [[nodiscard]] Status AddBuildBatch(std::span<const Part> batch) {
    if (finalized_) return Status::Invalid("Cannot add build rows after finalization");
    for (const auto& part : batch) {
      if (part.brand != selected_brand_) continue;
      ++matches_per_key_[part.part_key];
      bloom_.Insert(static_cast<uint64_t>(part.part_key));
    }
    return Status::OK();
  }

  /// @brief Seal all build input before any probe; repeated calls are harmless.
  void FinalizeBuild() noexcept { finalized_ = true; }

  /// @brief Probe one batch; exact lookup always verifies Bloom positives.
  /// @param batch Borrowed LINEITEM rows; a future Parquet adapter can supply these.
  /// @param use_bloom False runs the exact baseline for comparison.
  /// @return Output rows and counters, or Invalid if build is not finalized.
  [[nodiscard]] Result<ProbeResult> ProbeBatch(std::span<const Lineitem> batch,
                                              bool use_bloom = true) const {
    if (!finalized_) return Status::Invalid("Finalize build before probing");
    ProbeResult result;
    for (const auto& item : batch) {
      if (use_bloom && !bloom_.MayContain(static_cast<uint64_t>(item.part_key))) {
        ++result.bloom_rejected;
        continue;
      }
      ++result.exact_lookups;
      const auto match = matches_per_key_.find(item.part_key);
      if (match == matches_per_key_.end()) continue;
      for (std::size_t i = 0; i < match->second; ++i) result.rows.push_back(item);
    }
    return result;
  }

 private:
  TpchBloomJoin(std::string selected_brand, execution::BloomFilter bloom)
      : selected_brand_(std::move(selected_brand)), bloom_(std::move(bloom)) {}

  std::string selected_brand_;
  execution::BloomFilter bloom_;
  // Counts suffice because this example selects only l.*; preserve join multiplicity.
  std::unordered_map<int64_t, std::size_t> matches_per_key_;
  bool finalized_ = false;
};

}  // namespace quest::examples
