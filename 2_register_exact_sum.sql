-------------------------------------------
-- Usage:  vsql -ef 2_register_and_test_exact_sum.sql
-------------------------------------------

-- Create or replace the library object in Vertica that points to the shared library file on disk so the UDX code can be loaded.
CREATE OR REPLACE LIBRARY exact_sum_lib AS '/tmp/exact_sum.so';

-- Create or replace the aggregate function definition in Vertica so that the exact_sum UDX can be invoked in SQL queries.
CREATE OR REPLACE AGGREGATE FUNCTION exact_sum
AS LANGUAGE 'C++'
NAME 'ExactSumFactory'
LIBRARY exact_sum_lib;

-- Grant execute permission on the exact_sum aggregate function to all users, so everyone can call it without extra privileges.
GRANT EXECUTE ON AGGREGATE FUNCTION exact_sum(NUMERIC) TO PUBLIC;

