-- TPC-H SF 0.01, only the columns Q12 reads. Run from the repo root: duckdb < testdata/tpch/generate.sql
INSTALL tpch;
LOAD tpch;
CALL dbgen(sf = 0.01);
COPY (SELECT l_orderkey, l_shipmode, l_commitdate, l_receiptdate, l_shipdate FROM lineitem)
  TO 'testdata/tpch/lineitem.parquet' (FORMAT parquet);
COPY (SELECT o_orderkey, o_orderpriority FROM orders)
  TO 'testdata/tpch/orders.parquet' (FORMAT parquet);
-- Expected Q12 result (tests/test_tpch_q12.cpp):
--   MAIL | 64 | 86
--   SHIP | 61 | 96
PRAGMA tpch(12);
