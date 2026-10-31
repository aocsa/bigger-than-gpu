// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file bloom_join_demo.cpp
/// @brief Synthetic TPC-H projections: compare a Bloom-prefiltered join with its baseline.

#include <iostream>
#include <span>
#include <vector>

#include "bloom_join/tpch_bloom_join.hpp"

/// @brief Run an in-memory batched join and verify exact baseline equivalence.
/// @return Zero on equivalence, nonzero on errors.
int main() {
  using quest::examples::Lineitem;
  using quest::examples::Part;
  using quest::examples::TpchBloomJoin;

  // Small, deterministic fixtures inspired by TPC-H; these are not dbgen output.
  std::vector<Part> parts;
  std::vector<Lineitem> lineitems;
  for (int64_t key = 1; key <= 100; ++key) {
    parts.push_back({key, key % 10 == 0 ? "Brand#23" : "Brand#12"});
  }
  for (int64_t row = 0; row < 1000; ++row) {
    lineitems.push_back({row / 5 + 1, row % 100 + 1, 10000 + row});
  }

  auto join = TpchBloomJoin::Create("Brand#23");
  if (!join) {
    std::cerr << join.error().message_ << '\n';
    return 1;
  }
  const auto build = std::span<const Part>(parts);
  for (std::size_t offset = 0; offset < build.size(); offset += 25) {
    auto status = join->AddBuildBatch(build.subspan(offset, 25));
    if (!status) {
      std::cerr << status.error().message_ << '\n';
      return 1;
    }
  }
  join->FinalizeBuild();

  std::size_t output_rows = 0;
  std::size_t rejected = 0;
  std::size_t exact_lookups = 0;
  const auto probe = std::span<const Lineitem>(lineitems);
  for (std::size_t offset = 0; offset < probe.size(); offset += 100) {
    auto batch = probe.subspan(offset, 100);
    auto filtered = join->ProbeBatch(batch);
    auto baseline = join->ProbeBatch(batch, false);
    if (!filtered || !baseline || filtered->rows != baseline->rows) {
      std::cerr << "Join failed or differs from exact baseline\n";
      return 1;
    }
    output_rows += filtered->rows.size();
    rejected += filtered->bloom_rejected;
    exact_lookups += filtered->exact_lookups;
  }
  if (output_rows != 100) return 1;
  std::cout << "Synthetic TPC-H: PART JOIN LINEITEM, p_brand = 'Brand#23'\n"
            << "Probe rows: " << lineitems.size() << '\n'
            << "Bloom rejected: " << rejected << '\n'
            << "Exact lookups: " << exact_lookups << " (baseline: " << lineitems.size() << ")\n"
            << "Output rows: " << output_rows << '\n'
            << "Exact baseline comparison: PASS\n";
}
