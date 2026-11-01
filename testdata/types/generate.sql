-- Parquet files for the reader's type mapping tests (tests/test_parquet_reader.cpp).
-- Run from the repo root: duckdb < testdata/types/generate.sql
-- int96_timestamps.parquet comes from generate_int96.py (DuckDB cannot write INT96).
COPY (SELECT * FROM (VALUES
        (TIMESTAMP_MS '2024-01-02 03:04:05.5', TIMESTAMP '2024-01-02 03:04:05.123456',
         TIMESTAMP_NS '2024-01-02 03:04:05.123456789', 200::UTINYINT, 60000::USMALLINT,
         4000000000::UINTEGER, 1234567.89::DECIMAL(9, 2)),
        (NULL, TIMESTAMP '1969-12-31 23:59:59.999999', TIMESTAMP_NS '1969-12-31 23:59:59.999999999',
         NULL, NULL, NULL, -0.07::DECIMAL(9, 2)))
      t(ts_ms, ts_us, ts_ns, u8, u16, u32, dec32))
  TO 'testdata/types/timestamps_unsigned.parquet' (FORMAT parquet);
COPY (SELECT 1::UBIGINT AS u64) TO 'testdata/types/uint64.parquet' (FORMAT parquet);
COPY (SELECT '6d2a1f3e-58b0-4a0c-9e5b-1f2d3c4b5a69'::UUID AS id)
  TO 'testdata/types/uuid.parquet' (FORMAT parquet);
-- DuckDB writes infinity as INT64_MAX milliseconds, which overflows as microseconds.
COPY (SELECT 'infinity'::TIMESTAMP_MS AS t) TO 'testdata/types/timestamp_ms_infinity.parquet' (FORMAT parquet);
