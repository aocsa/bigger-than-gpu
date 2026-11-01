-- TPC-H SF 0.01, only the columns Q1, Q3, Q5, Q6, Q10 and Q12 read, plus DuckDB's answers.
-- Run from the repo root: duckdb < testdata/tpch/generate.sql
INSTALL tpch;
LOAD tpch;
CALL dbgen(sf = 0.01);
COPY (SELECT l_orderkey, l_suppkey, l_quantity, l_extendedprice, l_discount, l_tax, l_returnflag,
             l_linestatus, l_shipdate, l_commitdate, l_receiptdate, l_shipmode FROM lineitem)
  TO 'testdata/tpch/lineitem.parquet' (FORMAT parquet, ROW_GROUP_SIZE 20000);  -- 3 row groups
COPY (SELECT o_orderkey, o_custkey, o_orderdate, o_orderpriority, o_shippriority FROM orders)
  TO 'testdata/tpch/orders.parquet' (FORMAT parquet);
COPY (SELECT c_custkey, c_name, c_address, c_nationkey, c_phone, c_acctbal, c_mktsegment,
             c_comment FROM customer)
  TO 'testdata/tpch/customer.parquet' (FORMAT parquet);
COPY (SELECT s_suppkey, s_nationkey FROM supplier) TO 'testdata/tpch/supplier.parquet' (FORMAT parquet);
COPY (SELECT n_nationkey, n_name, n_regionkey FROM nation) TO 'testdata/tpch/nation.parquet' (FORMAT parquet);
COPY (SELECT r_regionkey, r_name FROM region) TO 'testdata/tpch/region.parquet' (FORMAT parquet);

-- Expected answers for tests/test_tpch.cpp, '|'-separated; DuckDB's tpch_queries() text.
COPY (SELECT l_returnflag, l_linestatus, sum(l_quantity) AS sum_qty,
             sum(l_extendedprice) AS sum_base_price,
             sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price,
             sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,
             avg(l_quantity) AS avg_qty, avg(l_extendedprice) AS avg_price,
             avg(l_discount) AS avg_disc, count(*) AS count_order
      FROM lineitem WHERE l_shipdate <= CAST('1998-09-02' AS date)
      GROUP BY l_returnflag, l_linestatus ORDER BY l_returnflag, l_linestatus)
  TO 'testdata/tpch/answers/q1.tbl' (DELIMITER '|', HEADER false, QUOTE '');
COPY (SELECT l_orderkey, sum(l_extendedprice * (1 - l_discount)) AS revenue, o_orderdate,
             o_shippriority
      FROM customer, orders, lineitem
      WHERE c_mktsegment = 'BUILDING' AND c_custkey = o_custkey AND l_orderkey = o_orderkey
        AND o_orderdate < CAST('1995-03-15' AS date) AND l_shipdate > CAST('1995-03-15' AS date)
      GROUP BY l_orderkey, o_orderdate, o_shippriority
      ORDER BY revenue DESC, o_orderdate LIMIT 10)
  TO 'testdata/tpch/answers/q3.tbl' (DELIMITER '|', HEADER false, QUOTE '');
COPY (SELECT n_name, sum(l_extendedprice * (1 - l_discount)) AS revenue
      FROM customer, orders, lineitem, supplier, nation, region
      WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND l_suppkey = s_suppkey
        AND c_nationkey = s_nationkey AND s_nationkey = n_nationkey
        AND n_regionkey = r_regionkey AND r_name = 'ASIA'
        AND o_orderdate >= CAST('1994-01-01' AS date) AND o_orderdate < CAST('1995-01-01' AS date)
      GROUP BY n_name ORDER BY revenue DESC)
  TO 'testdata/tpch/answers/q5.tbl' (DELIMITER '|', HEADER false, QUOTE '');
COPY (SELECT sum(l_extendedprice * l_discount) AS revenue
      FROM lineitem
      WHERE l_shipdate >= CAST('1994-01-01' AS date) AND l_shipdate < CAST('1995-01-01' AS date)
        AND l_discount BETWEEN 0.05 AND 0.07 AND l_quantity < 24)
  TO 'testdata/tpch/answers/q6.tbl' (DELIMITER '|', HEADER false, QUOTE '');
COPY (SELECT c_custkey, c_name, sum(l_extendedprice * (1 - l_discount)) AS revenue, c_acctbal,
             n_name, c_address, c_phone, c_comment
      FROM customer, orders, lineitem, nation
      WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey
        AND o_orderdate >= CAST('1993-10-01' AS date) AND o_orderdate < CAST('1994-01-01' AS date)
        AND l_returnflag = 'R' AND c_nationkey = n_nationkey
      GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, c_comment
      ORDER BY revenue DESC LIMIT 20)
  TO 'testdata/tpch/answers/q10.tbl' (DELIMITER '|', HEADER false, QUOTE '');
-- Expected Q12 result (tests/test_tpch_q12.cpp):
--   MAIL | 64 | 86
--   SHIP | 61 | 96
PRAGMA tpch(12);
