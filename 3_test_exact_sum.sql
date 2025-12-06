-------------------------------------
-- Usage:  vsql -f 3_stress_test_exact_sum.sql
-------------------------------------

\set DEMO_ROWS 1000
-- \set DEMO_ROWS 100000000

-- Drop any existing test table to ensure a clean environment before recreating it for this test.
drop table if exists public.my_numeric_test cascade;

-- Create a new test table with a single NUMERIC(75,2) column to hold very large decimal values for SUM testing.
create table public.my_numeric_test (
    row_id int,
    a numeric(75,2) default 1439324057017381289491464076569211292870045918343227178012190411543327443.13 + row_id
)
order by row_id
segmented by hash(row_id) ALL NODES;

INSERT INTO public.my_numeric_test
WITH myrows AS (
    SELECT
        row_number() over() AS row_id
    FROM (
        SELECT 1
        FROM (
            SELECT now() AS se
            UNION ALL
            SELECT now() + :DEMO_ROWS - 1 AS se
        ) a TIMESERIES ts AS '1 day' OVER (ORDER BY se)
    ) b
)
SELECT row_id
FROM myrows
ORDER BY row_id;
COMMIT;

\echo
\echo '##### Compute SUM(a); for these huge NUMERIC values Vertica's built-in SUM can overflow, so this result may be incorrect.'
SELECT SUM(a) AS built_in_sum FROM public.my_numeric_test;

\echo
\echo '##### Call exact_sum(a), which uses a much wider intermediate NUMERIC to produce a mathematically correct sum when possible.'
SELECT exact_sum(a) AS exact_sum FROM public.my_numeric_test;
\echo '^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^'

\echo
\echo '##### Compare built-in SUM(a) with exact_sum(a); a large gap indicates overflow in Vertica''s internal SUM, not an error in exact_sum.'
SELECT (SUM(a) - exact_sum(a)) as built_in_sum_minus_exact_sum
FROM public.my_numeric_test;

\echo
\echo '##### Compute the true mathematical sum for the arithmetic progression a = BASE + row_id,'
\echo '##### sum = (first + last) * n / 2, where first = MIN(a), last = MAX(a), n = COUNT(*).'
\echo '##### And verify exact_sum(a) matches this exact value with zero difference.'
SELECT
    (MIN(a)+MAX(a))*COUNT(*)/2               AS expected_sum,
    exact_sum(a)                             AS udx_sum,
    ((MIN(a)+MAX(a))*COUNT(*)/2 - exact_sum(a))::NUMERIC(80,5) AS diff
FROM public.my_numeric_test;

\x
\echo '##### In this test case, SUM(a) and exact_sum(a) agree for very large NUMERIC values, so there is no overflow here.'
DROP TABLE IF EXISTS public.my_numeric_demo CASCADE;
CREATE TABLE public.my_numeric_demo AS
SELECT 
    143932405701738128949146407656921129287004591834322717801219041154332744312345678901234567890123456789012345678901234567890.13 AS a
UNION ALL
SELECT 
    1.000000009
UNION ALL
SELECT 1.439E+1000
FROM dual;

\t
\echo '##### Calling SUM(a) on a very large number:'
SELECT sum(a) AS SUM FROM public.my_numeric_demo;

\echo '--- Calling exact_sum(a) on the same very large number:'
SELECT exact_sum(a) AS EXACT_SUM FROM public.my_numeric_demo;
\t

\echo '##### The SUM result is accurate here because the number of input rows is tiny and'
\echo '##### the running total never approaches the internal 256-bit accumulator limit.'
\echo '##### Even though the individual NUMERIC(1024,2) values are extremely large,'
\echo '##### overflow only depends on the accumulated magnitude, not on the raw size'
\echo '##### of each value. With just a few rows, the accumulator stays well within range,'
\echo '##### so SUM(a) returns a correct result.'

\echo ' '
\echo '##### Smallest row count N where the built-in SUM exceeds its internal 256-bit limit is 403 rows:'

WITH vals AS (
    SELECT
        ROW_NUMBER() OVER () AS row_id,
        CAST(
            1439324057017381289491464076569211292870045918343227178012190411543327443.13
            + ROW_NUMBER() OVER ()
            AS NUMERIC(75,2)
        ) AS a
    FROM (
        SELECT 1
        FROM (
            SELECT NOW() AS se
            UNION ALL
            SELECT NOW() + 1000 - 1 AS se
        ) a TIMESERIES ts AS '1 day' OVER (ORDER BY se)
    ) b
),
prefix AS (
    SELECT row_id AS n_rows FROM vals
),
agg AS (
    SELECT
        p.n_rows,
        MIN(v.a) AS first_value,
        MAX(v.a) AS last_value,
        SUM(v.a) AS built_in_sum,
        exact_sum(v.a) AS exact_sum,
        SUM(v.a) - exact_sum(v.a) AS gap,
        (MIN(v.a) + MAX(v.a)) * COUNT(*) / 2 AS expected_sum
    FROM vals v
    JOIN prefix p ON v.row_id <= p.n_rows
    GROUP BY p.n_rows
),
bounds AS (
    SELECT
        MAX(CASE WHEN gap = 0 THEN n_rows END) AS max_ok,
        MIN(CASE WHEN gap <> 0 THEN n_rows END) AS first_bad
    FROM agg
)
SELECT
    'correct_until_here' AS boundary_kind,
    n_rows, built_in_sum, exact_sum, expected_sum, gap
FROM agg WHERE n_rows = (SELECT max_ok FROM bounds)

UNION ALL

SELECT
    'first_overflow' AS boundary_kind,
    n_rows, built_in_sum, exact_sum, expected_sum, gap
FROM agg WHERE n_rows = (SELECT first_bad FROM bounds)

ORDER BY n_rows;


\echo
\echo '##### ===== SUMMARY ====='
\echo '##### The reported gap value -1157920892373161954235709850086879078532699846656405640394575840079131296399.36'
\echo '##### is exactly -2^256 / 100 when we compute it numerically, which matches the idea that Vertica’s SUM()'
\echo '##### for this NUMERIC(75,2) pattern is using an internal accumulator equivalent to a 256-bit integer scaled by 10².'
\echo '##### For n_rows = 402, the true mathematical sum is still within the positive range of that accumulator,'
\echo '##### so SUM(a) and exact_sum(a) agree and gap = 0.'
\echo '##### When we move to n_rows = 403, the true sum crosses that internal limit, the accumulator wraps once modulo 2^256,'
\echo '##### and the result is exactly one “wrap amount” (2²⁵⁶/100) lower than the mathematically correct value,'
\echo '##### hence the large negative constant gap that appears for 403 and then stays constant as we keep adding rows:'
\echo '##### built_in_sum = exact_sum - 2^256/100  → large negative decimal'
\echo '##### ==================='

