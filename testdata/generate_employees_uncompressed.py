#!/usr/bin/env python3
"""Generate uncompressed Parquet file for Employees table with id, salary, tax, name."""

import pyarrow as pa
import pyarrow.parquet as pq
import random
import string

# Seed for reproducibility
random.seed(123)

def random_name(length=10):
    """Generate a random name."""
    return ''.join(random.choices(string.ascii_letters, k=length)).capitalize()

# Configuration
NUM_ROW_GROUPS = 100
ROWS_PER_GROUP = 1000  # Total: 100,000 rows

print("Generating Employees (uncompressed) table...")

employee_batches = []
for rg in range(NUM_ROW_GROUPS):
    start_id = rg * ROWS_PER_GROUP + 1

    ids = list(range(start_id, start_id + ROWS_PER_GROUP))
    salaries = [random.randint(30000, 250000) for _ in range(ROWS_PER_GROUP)]
    taxes = [int(salary * random.uniform(0.15, 0.35)) for salary in salaries]
    names = [random_name() for _ in range(ROWS_PER_GROUP)]

    batch = pa.RecordBatch.from_pydict({
        'id': pa.array(ids, type=pa.int64()),
        'salary': pa.array(salaries, type=pa.int64()),
        'tax': pa.array(taxes, type=pa.int64()),
        'name': pa.array(names, type=pa.string()),
    })
    employee_batches.append(batch)

# Write employees table with NO compression
output_path = 'employees_uncompressed.parquet'
with pq.ParquetWriter(
    output_path,
    employee_batches[0].schema,
    compression='none',
) as writer:
    for batch in employee_batches:
        writer.write_batch(batch)

print(f"  Written: {output_path}")
print(f"  Row groups: {NUM_ROW_GROUPS}")
print(f"  Total rows: {NUM_ROW_GROUPS * ROWS_PER_GROUP}")

# Verify
pf = pq.ParquetFile(output_path)
print(f"  Verified row groups: {pf.metadata.num_row_groups}")
print(f"  Compression: {pf.metadata.row_group(0).column(0).compression}")

print("\nSchema:")
print(pq.read_schema(output_path))

print("\nSample data (first 5 rows):")
sample = pq.read_table(output_path).slice(0, 5)
for i in range(sample.num_rows):
    row = {col: sample.column(col)[i].as_py() for col in sample.column_names}
    print(f"  {row}")

print("\nDone!")
