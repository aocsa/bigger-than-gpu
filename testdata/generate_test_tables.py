#!/usr/bin/env python3
"""Generate test Parquet files for Employees and Departments tables.

Creates files with 100 row groups each, using Snappy compression.
"""

import pyarrow as pa
import pyarrow.parquet as pq
import random
import string

# Seed for reproducibility
random.seed(42)

def random_name(length=10):
    """Generate a random name."""
    return ''.join(random.choices(string.ascii_letters, k=length)).capitalize()

def random_department_name():
    """Generate a random department name."""
    prefixes = ["Engineering", "Sales", "Marketing", "Finance", "HR", "Legal",
                "Operations", "Research", "Support", "Product", "Design", "Data",
                "Security", "Infrastructure", "QA", "DevOps", "Analytics", "Admin"]
    suffixes = ["", " Team", " Division", " Group", " Department"]
    return random.choice(prefixes) + random.choice(suffixes)

def random_location():
    """Generate a random location name."""
    cities = ["New York", "San Francisco", "London", "Tokyo", "Berlin", "Paris",
              "Sydney", "Toronto", "Singapore", "Mumbai", "Seattle", "Austin",
              "Boston", "Chicago", "Denver", "Miami", "Atlanta", "Portland"]
    return random.choice(cities)

# Configuration
NUM_ROW_GROUPS = 100
ROWS_PER_GROUP = 1000  # Total: 100,000 rows per table

# =============================================================================
# Generate Employees Table
# =============================================================================
print("Generating Employees table...")

employee_batches = []
for rg in range(NUM_ROW_GROUPS):
    start_id = rg * ROWS_PER_GROUP + 1

    ids = list(range(start_id, start_id + ROWS_PER_GROUP))
    names = [random_name() for _ in range(ROWS_PER_GROUP)]
    department_ids = [random.randint(1, 500) for _ in range(ROWS_PER_GROUP)]
    salaries = [random.randint(30000, 250000) for _ in range(ROWS_PER_GROUP)]

    batch = pa.RecordBatch.from_pydict({
        'id': pa.array(ids, type=pa.int64()),
        'name': pa.array(names, type=pa.string()),
        'department_id': pa.array(department_ids, type=pa.int32()),
        'salary': pa.array(salaries, type=pa.int64()),
    })
    employee_batches.append(batch)

# Write employees table with 100 row groups
employees_path = 'employees.parquet'
with pq.ParquetWriter(
    employees_path,
    employee_batches[0].schema,
    compression='snappy',
) as writer:
    for batch in employee_batches:
        writer.write_batch(batch)

print(f"  Written: {employees_path}")
print(f"  Row groups: {NUM_ROW_GROUPS}")
print(f"  Total rows: {NUM_ROW_GROUPS * ROWS_PER_GROUP}")

# Verify
pf = pq.ParquetFile(employees_path)
print(f"  Verified row groups: {pf.metadata.num_row_groups}")
print(f"  Compression: {pf.metadata.row_group(0).column(0).compression}")

# =============================================================================
# Generate Departments Table
# =============================================================================
print("\nGenerating Departments table...")

department_batches = []
for rg in range(NUM_ROW_GROUPS):
    start_id = rg * ROWS_PER_GROUP + 1

    ids = list(range(start_id, start_id + ROWS_PER_GROUP))
    names = [random_department_name() for _ in range(ROWS_PER_GROUP)]
    budgets = [random.randint(100000, 10000000) for _ in range(ROWS_PER_GROUP)]
    location_ids = [random.randint(1, 100) for _ in range(ROWS_PER_GROUP)]

    batch = pa.RecordBatch.from_pydict({
        'id': pa.array(ids, type=pa.int64()),
        'name': pa.array(names, type=pa.string()),
        'budget': pa.array(budgets, type=pa.int64()),
        'location_id': pa.array(location_ids, type=pa.int32()),
    })
    department_batches.append(batch)

# Write departments table with 100 row groups
departments_path = 'departments.parquet'
with pq.ParquetWriter(
    departments_path,
    department_batches[0].schema,
    compression='snappy',
) as writer:
    for batch in department_batches:
        writer.write_batch(batch)

print(f"  Written: {departments_path}")
print(f"  Row groups: {NUM_ROW_GROUPS}")
print(f"  Total rows: {NUM_ROW_GROUPS * ROWS_PER_GROUP}")

# Verify
pf = pq.ParquetFile(departments_path)
print(f"  Verified row groups: {pf.metadata.num_row_groups}")
print(f"  Compression: {pf.metadata.row_group(0).column(0).compression}")

# =============================================================================
# Print Schema Info
# =============================================================================
print("\n" + "="*60)
print("Employees Schema:")
print(pq.read_schema(employees_path))

print("\nDepartments Schema:")
print(pq.read_schema(departments_path))

print("\n" + "="*60)
print("Sample data from Employees (first 5 rows):")
employees_sample = pq.read_table(employees_path).slice(0, 5)
for i in range(employees_sample.num_rows):
    row = {col: employees_sample.column(col)[i].as_py() for col in employees_sample.column_names}
    print(f"  {row}")

print("\nSample data from Departments (first 5 rows):")
departments_sample = pq.read_table(departments_path).slice(0, 5)
for i in range(departments_sample.num_rows):
    row = {col: departments_sample.column(col)[i].as_py() for col in departments_sample.column_names}
    print(f"  {row}")

print("\nDone!")
