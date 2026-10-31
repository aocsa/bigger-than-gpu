# SF200 Benchmark: 1-GPU vs 2-GPU (SNMG) on 2× H100 SXM5

Compares TPC-H SF200 (~200 GB raw, 22 queries) running on **one** H100 SXM5 against
**both** H100s (SNMG — Single Node Multi-GPU) using the `repro/sf1000-8.5s` branch.
Everything except `num_gpus` and the prefetcher flag is kept identical, so the
difference in results is purely attributable to the extra GPU.

> This document adapts [`repro-sf1000.md`](repro-sf1000.md) for a 2-GPU H100 node.
> Read that doc first for background on the branch changes and gotchas.

---

## Target machine

| Resource | This node | Notes |
|---|---|---|
| GPU | 2× H100 SXM5 80 GB | 160 GB total HBM3, sm_90 |
| GPU interconnect | NVLink 4.0 | 900 GB/s bidirectional P2P |
| GPU HBM bandwidth | 3.35 TB/s per GPU | 6.7 TB/s aggregate |
| Host RAM | 450 GB | ~94 GB used as pinned host pool |
| Disk | ≥ 400 GB free | NVMe preferred |
| CUDA | 12.x+ | sm_90 requires CUDA ≥ 12.0 |
| OS | Linux (Ubuntu 22.04 / 24.04) | |

### Why SF200 fits comfortably on either 1 or 2 H100s

SF1000 peaks at 251.7 GB of compressed GPU-resident data on a single 256 GB GB300.
SF200 is 1/5 the data — **~50 GB total compressed**. On one H100 (80 GB) that is ~63%
utilization; on two H100s it is ~25 GB per GPU (~31%). Neither configuration will
trigger the downgrade executor at SF200.

---

## How Sirius distributes work across 2 GPUs (2-GPU run only)

| Layer | Behavior |
|---|---|
| **Scan dispatch** | Round-robin: each scan batch gets `preferred_device = batch_idx % 2`. Lineitem chunks alternate between GPU 0 and GPU 1. |
| **Partitioned operators** (hash join, grouped aggregate) | Partition count floored to `num_gpus`. Each partition pinned to `partition_idx % 2`. The hash table for partition 0 lives on GPU 0 and never moves. |
| **Cross-GPU movement** | `cudaMemcpyPeerAsync` over NVLink 4.0 (900 GB/s). Falls back to host-staged copy silently if P2P is unavailable. |
| **Scheduling** | Two independent `gpu_pipeline_executor` instances. Device-pinned tasks go to their GPU; no-preference tasks go to whichever GPU signals ready first. |
| **No NCCL / no all-reduce** | All coordination is point-to-point cucascade batch conversion. |

Confirm NVLink P2P is active before running:

```bash
nvidia-smi nvlink --status -i 0
# Must show active lanes — otherwise cross-GPU transfers fall back to PCIe (~48 GB/s)
```

At init, Sirius logs P2P status. Look for:

```
[info] cudaDeviceCanAccessPeer(0 → 1): true  → peer access enabled
[info] cudaDeviceCanAccessPeer(1 → 0): true  → peer access enabled
```

---

## Steps 1–3 — Build and generate data (same for both runs)

### Step 1 — Check out and build Sirius

```bash
git checkout repro/sf1000-8.5s
git submodule update --init --recursive
pixi run make
```

### Step 2 — Build the patched libcudf

```bash
pixi run bash bench/sf1000-repro/build-libcudf.sh
# Output: $HOME/cudf-src/cpp/build/libcudf.so
# CUDA arch: CMAKE_CUDA_ARCHITECTURES=NATIVE → sm_90 on H100
# Build time: 25–50 min
```

| Patch | Effect |
|---|---|
| `strings::like` backtrack skip | q13 −36.5% |
| `cuda_memcpy` 2 MiB threshold | q9 −5.8% |
| groupby shared-memory replication | q1 ~−5% |

### Step 3 — Generate the SF200 dataset

```bash
cd test/tpch_performance
pixi run bash generate_tpch_data.sh 200 \
  --output ~/tpch_parquet_sf200 --jobs 16
cd ../..
```

Output: `~/tpch_parquet_sf200/` (~200 GB raw). Generation time: ~5–10 min.

> **Do not use `CALL dbgen(sf=200)`** — DuckDB produces 122K-row row groups.
> The `tpchgen-rs` path writes 2M–10M-row groups required for GPU scan efficiency.

---

## Step 4 — Configs

Two configs, one for each run. They are **identical except for `num_gpus` and
`memory_prefetcher.enable`** — the only parameters that must change between runs.

### 4a — Single GPU (`bench/sf200-1gpu/sirius-sf200-1gpu.yaml`)

```yaml
sirius:
  topology:
    num_gpus: 1                   # single H100 — uses GPU 0

  memory:
    gpu:
      # One H100 at 80 GB; SF200 compressed pins ~50 GB total (~63% utilization).
      usage_limit_fraction:       0.95   # 76 GB usable
      reservation_limit_fraction: 1.0
      downgrade_trigger_fraction: 0.9    # trigger at ~68 GB — won't fire at SF200
      downgrade_stop_fraction:    0.85

    host:
      capacity_bytes:       94240000000  # ~94 GB pinned host pool
      initial_number_pools: 80
      pool_size:            8
      block_size:           67108864     # 64 MiB
      memory_prefetcher:
        enable:      true              # safe with num_gpus=1
        num_threads: 3

    disk:
      disk_id: 0
      capacity_bytes: 200000000000
      downgrade_root_dirs: ""

  executor:
    scan_manager:
      num_threads:           18
      use_sirius_datasource: true
      uring_n_reactors:      4
      enable_prefetch_cache: false
      memory_prefetcher:
        enable:      true              # safe with num_gpus=1
        num_threads: 3
    pipeline:
      num_threads: 8
    downgrade:
      num_threads: 1
    task_creator:
      num_threads: 4

  operator_params:
    scan_task_batch_size:       8GB
    max_sort_partition_bytes:   0
    hash_partition_bytes:       16GB
    concat_batch_bytes:         2GB
    max_build_hash_table_bytes: 16GB

  telemetry:
    enable_quent:     false
    output_directory: telemetry_data
    engine_name:      siriusDB
```

### 4b — Two GPUs / SNMG (`bench/sf200-2gpu/sirius-sf200-2gpu.yaml`)

```yaml
sirius:
  topology:
    num_gpus: 2                   # SNMG: both H100s

  memory:
    gpu:
      # Applied per-GPU. Each H100 at 80 GB; SF200 pins ~25 GB per GPU (~31%).
      usage_limit_fraction:       0.95
      reservation_limit_fraction: 1.0
      downgrade_trigger_fraction: 0.9
      downgrade_stop_fraction:    0.85

    host:
      capacity_bytes:       94240000000  # same total — shared across both GPUs
      initial_number_pools: 80
      pool_size:            8
      block_size:           67108864     # 64 MiB
      memory_prefetcher:
        enable:      false             # MUST be false for num_gpus > 1
        num_threads: 3

    disk:
      disk_id: 0
      capacity_bytes: 200000000000
      downgrade_root_dirs: ""

  executor:
    scan_manager:
      num_threads:           18
      use_sirius_datasource: true      # forced true by engine on multi-GPU; set explicitly to suppress WARN
      uring_n_reactors:      4
      enable_prefetch_cache: false
      memory_prefetcher:
        enable:      false             # MUST be false for num_gpus > 1
        num_threads: 3
    pipeline:
      num_threads: 8
    downgrade:
      num_threads: 1
    task_creator:
      num_threads: 4

  operator_params:
    scan_task_batch_size:       8GB
    max_sort_partition_bytes:   0
    hash_partition_bytes:       16GB
    concat_batch_bytes:         2GB
    max_build_hash_table_bytes: 16GB

  telemetry:
    enable_quent:     false
    output_directory: telemetry_data
    engine_name:      siriusDB
```

### Config diff — what changes between the two runs

| Parameter | 1-GPU | 2-GPU | Why |
|---|---|---|---|
| `topology.num_gpus` | **1** | **2** | the variable under test |
| `memory_prefetcher.enable` (both locations) | **true** | **false** | prefetcher is single-GPU only; engine disables it with WARN on multi-GPU |
| `use_sirius_datasource` comment | optional | set explicitly | engine forces it true on multi-GPU and logs WARN if not set |
| Everything else | identical | identical | controlled experiment |

---

## Step 5 — Run scripts

### 5a — Single GPU (`bench/sf200-1gpu/run.sh`)

```bash
#!/usr/bin/env bash
# TPC-H SF200, single H100 (GPU 0 only)
# Run from repo root:  pixi run bash bench/sf200-1gpu/run.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

DATA="${DATA:-$HOME/tpch_parquet_sf200}"
CUDF_SO="${CUDF_SO:-$HOME/cudf-src/cpp/build/libcudf.so}"
PLANS="${PLANS:-$REPO/bench/sf1000-repro/plans}"
CFG="${CFG:-$HERE/sirius-sf200-1gpu.yaml}"
NAME="${NAME:-sf200_1gpu}"

[ -d "$DATA" ]    || { echo "ERROR: no SF200 parquet at $DATA (set DATA=)"; exit 1; }
[ -f "$CUDF_SO" ] || { echo "ERROR: no patched libcudf at $CUDF_SO -- run build-libcudf.sh first"; exit 1; }

export LD_PRELOAD="$CUDF_SO"
export SIRIUS_PRE_SQL="SET pin_table_compression = true; \
SET pin_table_input_compression_plan_dir = '$PLANS'; \
SET expression_evaluator_strategy = 'ast_jit'"

for t in LINEITEM ORDERS PART CUSTOMER SUPPLIER NATION REGION PARTSUPP; do
  export "SIRIUS_PIN_TIER_$t=gpu"
done

python3 "$REPO/test/tpch_performance/performance_test.py" \
  --input "$DATA" \
  --mode grouped --iterations 3 --engine gpu --pin host \
  --queries 1-22 --config "$CFG" --name "$NAME"
```

### 5b — Two GPUs / SNMG (`bench/sf200-2gpu/run.sh`)

```bash
#!/usr/bin/env bash
# TPC-H SF200, SNMG on 2× H100 SXM5
# Run from repo root:  pixi run bash bench/sf200-2gpu/run.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

DATA="${DATA:-$HOME/tpch_parquet_sf200}"
CUDF_SO="${CUDF_SO:-$HOME/cudf-src/cpp/build/libcudf.so}"
PLANS="${PLANS:-$REPO/bench/sf1000-repro/plans}"
CFG="${CFG:-$HERE/sirius-sf200-2gpu.yaml}"
NAME="${NAME:-sf200_2gpu}"

[ -d "$DATA" ]    || { echo "ERROR: no SF200 parquet at $DATA (set DATA=)"; exit 1; }
[ -f "$CUDF_SO" ] || { echo "ERROR: no patched libcudf at $CUDF_SO -- run build-libcudf.sh first"; exit 1; }

export LD_PRELOAD="$CUDF_SO"
export SIRIUS_PRE_SQL="SET pin_table_compression = true; \
SET pin_table_input_compression_plan_dir = '$PLANS'; \
SET expression_evaluator_strategy = 'ast_jit'"

for t in LINEITEM ORDERS PART CUSTOMER SUPPLIER NATION REGION PARTSUPP; do
  export "SIRIUS_PIN_TIER_$t=gpu"
done

python3 "$REPO/test/tpch_performance/performance_test.py" \
  --input "$DATA" \
  --mode grouped --iterations 3 --engine gpu --pin host \
  --queries 1-22 --config "$CFG" --name "$NAME"
```

The run scripts are identical except for `CFG` and `NAME` — the config file carries
all the behavioral differences.

---

## Step 6 — Validate results (run once before timing)

Byte-compare GPU output against DuckDB CPU to confirm correctness before spending
time on timing runs. Run validation for **both configs**.

```bash
# Set up env once
export LD_PRELOAD="$HOME/cudf-src/cpp/build/libcudf.so"
export SIRIUS_PRE_SQL="SET pin_table_compression = true; \
  SET pin_table_input_compression_plan_dir = 'bench/sf1000-repro/plans'; \
  SET expression_evaluator_strategy = 'ast_jit'"
for t in LINEITEM ORDERS PART CUSTOMER SUPPLIER NATION REGION PARTSUPP; do
  export "SIRIUS_PIN_TIER_$t=gpu"
done

# 1-GPU validation
pixi run python3 test/tpch_performance/performance_test.py \
  --input ~/tpch_parquet_sf200 --mode grouped --iterations 1 \
  --engine both --validation --queries 1-22 \
  --config bench/sf200-1gpu/sirius-sf200-1gpu.yaml \
  --name sf200_1gpu_validation

# 2-GPU validation
pixi run python3 test/tpch_performance/performance_test.py \
  --input ~/tpch_parquet_sf200 --mode grouped --iterations 1 \
  --engine both --validation --queries 1-22 \
  --config bench/sf200-2gpu/sirius-sf200-2gpu.yaml \
  --name sf200_2gpu_validation
```

Both should pass all 22 queries. Failure on the same query in both configs points to
a data or plan issue; failure only in the 2-GPU run points to a multi-GPU correctness
regression.

---

## Step 7 — Run and compare

Run both benchmarks back-to-back. Order matters: run 1-GPU first so the machine is
in a consistent state for both (no cached GPU state from a prior 2-GPU run).

```bash
pixi run bash bench/sf200-1gpu/run.sh
pixi run bash bench/sf200-2gpu/run.sh
```

### Compare totals

```bash
for cfg in 1gpu 2gpu; do
  printf "sf200_%s: " "$cfg"
  awk -F',' 'NR>1 { sum += $3 } END { printf "%.3f s total\n", sum }' \
    "test/tpch_performance/output/sf200_${cfg}/csv/runtimes.csv"
done
```

### Per-query breakdown

```bash
python3 - <<'EOF'
import csv, sys

def load(path):
    best = {}
    with open(path) as f:
        for row in csv.DictReader(f):
            q = int(row['query'])
            t = float(row['best'] if 'best' in row else row.get('time', row.get('elapsed',0)))
            best[q] = min(best.get(q, 1e9), t)
    return best

r1 = load("test/tpch_performance/output/sf200_1gpu/csv/runtimes.csv")
r2 = load("test/tpch_performance/output/sf200_2gpu/csv/runtimes.csv")

print(f"{'Q':>3}  {'1-GPU (s)':>10}  {'2-GPU (s)':>10}  {'speedup':>8}")
print("-" * 40)
for q in sorted(set(r1) | set(r2)):
    t1 = r1.get(q, float('nan'))
    t2 = r2.get(q, float('nan'))
    sp = t1 / t2 if t2 > 0 else float('nan')
    flag = " ◄" if sp > 1.3 else ("  !" if sp < 0.85 else "")
    print(f"Q{q:02d}  {t1:>10.3f}  {t2:>10.3f}  {sp:>7.2f}×{flag}")

t1_tot = sum(r1.values()); t2_tot = sum(r2.values())
print("-" * 40)
print(f"{'TOT':>3}  {t1_tot:>10.3f}  {t2_tot:>10.3f}  {t1_tot/t2_tot:>7.2f}×")
EOF
```

`◄` marks queries where 2-GPU is ≥ 1.3× faster (scan/join parallelism).
`!` marks queries where 2-GPU is slower (coordination overhead dominates on small tables).

### Verify 2-GPU used both GPUs

```bash
grep -E "assigned to GPU [01]|preferred_device=[01]|device=[01]" \
  test/tpch_performance/output/sf200_2gpu/log_dir/*.log \
  | awk -F'GPU ' '{print $2}' | sort | uniq -c
# Should show roughly equal counts for GPU 0 and GPU 1
```

---

## Pinning strategy: why `--mode grouped` is used for both runs

At SF200 neither mode causes downgrade (63% per GPU for 1-GPU sequential; 31% per GPU
for 2-GPU sequential). `--mode grouped` is used anyway to keep methodology identical
to the SF1000 baseline and to avoid holding stale pinned batches in GPU memory between
queries.

| Config | Mode | GPU peak | Downgrade? |
|---|---|---|---|
| 1-GPU | grouped | ~50 GB / 80 GB (63%) | No |
| 1-GPU | sequential | ~50 GB / 80 GB (63%) | No |
| 2-GPU | grouped | ~25 GB / 80 GB (31%) | No |
| 2-GPU | sequential | ~25 GB / 80 GB (31%) | No |

---

## Gotchas

### All runs

**`l_shipinstruct` must stay `dictionary`** in `bench/sf1000-repro/plans/lineitem.txt`.
Q19 decode-time predicate pushdown requires it. Changing to `identity` silently costs
~21% on Q19.

**Use `LD_PRELOAD`, not `LD_LIBRARY_PATH`** — pixi's unpatched cuDF loads first with
`LD_LIBRARY_PATH`, defeating the patches.

**JIT warm-up adds ~19 s to the first query** in a cold process. Best-of-3 absorbs
this; never report iteration 1 from a fresh start.

### 2-GPU run only

**`memory_prefetcher.enable` must be `false`** in both `memory.host.memory_prefetcher`
and `executor.scan_manager.memory_prefetcher`. The engine disables it with a WARN if
left true; set it explicitly to suppress log noise.

**`use_sirius_datasource` is forced `true`** when `num_gpus > 1`. The engine sets it
with a WARN; explicit `true` in the config suppresses the log.

**Hash-join retry warnings are expected** and not fatal:

```
[warn] invalid hash table build state 2 — retrying (attempt N/100)
```

The probe task on GPU 1 can arrive before the build task on GPU 0 completes.
`MAX_RETRIES=100` with 50 ms backoff handles this. If retries exceed 100, check
`pipeline.num_threads` and GPU utilization.

**NVLink P2P must be active** for cross-GPU join performance. Verify with
`nvidia-smi nvlink --status -i 0` before running. PCIe fallback (~48 GB/s)
will make join-heavy queries (q3, q5, q7, q8, q9, q10) substantially slower.

---

## Quick-start checklist

```
[ ] git checkout repro/sf1000-8.5s && git submodule update --init --recursive
[ ] pixi run make
[ ] pixi run bash bench/sf1000-repro/build-libcudf.sh
[ ] nvidia-smi nvlink --status -i 0          # confirm NVLink active (2-GPU only)
[ ] pixi run bash test/tpch_performance/generate_tpch_data.sh 200 \
      --output ~/tpch_parquet_sf200 --jobs 16
[ ] mkdir -p bench/sf200-1gpu bench/sf200-2gpu
    # write sirius-sf200-1gpu.yaml + run.sh  (Step 4a / 5a above)
    # write sirius-sf200-2gpu.yaml + run.sh  (Step 4b / 5b above)
[ ] # Validate both configs first
    pixi run bash bench/sf200-1gpu/run.sh    # --validation pass
    pixi run bash bench/sf200-2gpu/run.sh    # --validation pass
[ ] # Timing runs
    pixi run bash bench/sf200-1gpu/run.sh
    pixi run bash bench/sf200-2gpu/run.sh
[ ] # Compare
    for cfg in 1gpu 2gpu; do
      printf "sf200_%s: " "$cfg"
      awk -F',' 'NR>1{sum+=$3}END{printf "%.3f s\n",sum}' \
        test/tpch_performance/output/sf200_${cfg}/csv/runtimes.csv
    done
```

