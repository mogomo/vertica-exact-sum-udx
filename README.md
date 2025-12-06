# exact_sum – High-Precision SUM() UDX for Vertica

## 1. Overview

`exact_sum` is a Vertica User-Defined Aggregate Function (UDAF) that computes
mathematically exact sums for `NUMERIC(p, s)` columns whenever the true sum
can be represented within Vertica’s `NUMERIC(1024, s)` domain. When that is
not possible, it raises a clear error instead of returning a result whose
precision cannot be guaranteed.

Vertica’s built‑in aggregates work well for typical workloads. `exact_sum`
is intended for rare, extreme-value scenarios that need additional numeric
safety and explicit diagnostics.

---

## 2. What `exact_sum` does

**Function signature**

```sql
exact_sum(a NUMERIC(p_in, s_in)) RETURNS NUMERIC(p_out, s_out)
```

Where the output type is chosen as:

```text
p_out = min(1024, p_in + 19)
s_out = clamp(s_in, 0, p_out)
```

This means:

- The output keeps the input scale (limited to [0, `p_out`]).
- Up to 19 extra digits of precision are added to accommodate large row counts.
- The output type adapts automatically to the input column’s precision and scale.

`exact_sum`:

- Ignores NULLs (same semantics as built‑in `SUM`).
- Returns NULL if all values in a group are NULL.
- Either returns the mathematically correct sum or raises a diagnostic error
  if that cannot be guaranteed within Vertica’s numeric limits.

---

## 3. Internal approach (high level)

The intermediate state maintained by `exact_sum` contains:

- A wide NUMERIC accumulator for the running sum:

  ```text
  p_sum = min(1024, p_in + 19)
  s_sum = clamp(s_in, 0, p_sum)
  ```

- The row count (`cnt`).
- The input precision and scale (`p_in`, `s_in`).

The extra 19 digits in `p_sum` are chosen because, for any 64‑bit row count
`N ≤ 9,223,372,036,854,775,807`, we have `ceil(log10(N)) ≤ 19`. This is the
maximum number of extra digits needed so that, whenever an exact sum is
representable within `NUMERIC(1024, s)`, the accumulator has enough precision.

**Finalization logic (terminate phase)**

At the end of aggregation for each group, `exact_sum`:

1. Computes the number of decimal digits in the row count `rowCount`.
2. Computes the worst‑case precision needed for the sum:

   ```text
   p_needed = p_in + digits(rowCount)
   ```

3. If `p_needed > 1024`, then no exact sum can be represented within
   Vertica’s numeric limit for this input type and group size, and the
   function raises a clear error explaining the situation.

4. Otherwise, the accumulated sum is exactly representable in the chosen
   intermediate type (`p_sum` is at least `p_needed`), and the result is
   copied to the output. In this case, the returned value is the
   mathematically correct sum of all non‑NULL inputs in the group.

---

## 4. Repository contents

| File                     | Description                                                                 |
|--------------------------|-----------------------------------------------------------------------------|
| `exact_sum.cpp`          | UDX implementation using the Vertica SDK                                   |
| `Makefile_exact_sum`     | Makefile that builds `/tmp/exact_sum.so`                                   |
| `1_compile_exact_sum.sh` | Helper script that runs the Makefile and produces `/tmp/exact_sum.so`      |
| `2_register_exact_sum.sql` | SQL script to register the library and `exact_sum` aggregate in Vertica |
| `3_test_exact_sum.sql`   | SQL script that demonstrates behavior and validates correctness on large `NUMERIC` values |

---

## 5. Build instructions

Run on a Vertica node where the SDK and build tools are available:

```bash
./1_compile_exact_sum.sh
```

This will:

- Clean any previous build of `/tmp/exact_sum.so` using `Makefile_exact_sum`.
- Compile `exact_sum.cpp` into `/tmp/exact_sum.so`.

If you prefer to invoke `make` directly:

```bash
make -f Makefile_exact_sum clean
make -f Makefile_exact_sum
```

---

## 6. Register the UDX in Vertica

Use `vsql` to create the library object and aggregate function:

```bash
vsql -ef 2_register_exact_sum.sql
```

This script:

1. Creates or replaces a library object `exact_sum_lib` that points to `/tmp/exact_sum.so`.
2. Creates or replaces the `exact_sum` aggregate function based on `ExactSumFactory`.
3. Grants `EXECUTE` privilege on `exact_sum(NUMERIC)` to `PUBLIC` so all users can call it.

You can adjust the path to the shared object or privileges in the SQL script if needed for your environment.

---

## 7. Test and demonstration

Run the test script:

```bash
vsql -ef 3_test_exact_sum.sql
```

The script:

- Creates a table with 1,000 very large `NUMERIC(75,2)` values.
- Compares the built‑in `SUM(a)` with `exact_sum(a)`.
- Computes an analytical “expected” sum for an arithmetic progression and verifies
  that `exact_sum(a)` matches it exactly.
- Demonstrates a small example with extremely large `NUMERIC(1024,2)` values where
  both `SUM(a)` and `exact_sum(a)` agree, because the accumulated total stays well
  within the internal limits for that case.
- Finds the smallest number of rows in a specific test pattern (403 rows) where the
  built‑in `SUM(a)` no longer matches `exact_sum(a)`, illustrating how `exact_sum`
  can be used to detect and quantify cases where internal accumulation limits are
  exceeded for very large numeric ranges.

These tests are illustrative; you should repeat similar checks on your own data and
schemas to validate behavior for your workload.

---

## 8. Using `exact_sum` in your own queries

Basic usage:

```sql
SELECT exact_sum(a) FROM big_table;

SELECT customer_id, exact_sum(order_total)
FROM orders
GROUP BY customer_id;
```

Notes:

- NULL values are ignored, following standard SQL `SUM` semantics.
- If all values in a group are NULL, `exact_sum` returns NULL.
- When the required precision exceeds Vertica’s numeric limit, `exact_sum`
  raises a descriptive error so you know that no exact sum is possible for
  that combination of type and group size.

---

## 9. Notes

- `exact_sum` respects Vertica’s global `NUMERIC(1024, s)` precision limit.
- It is designed for specialized, high‑precision use cases rather than
  everyday aggregation.
- When an exact result is representable, it is returned; when it is not,
  a clear diagnostic error explains why.

---

## 10. Summary

`exact_sum` provides:

- Dynamic, input‑aware precision selection.
- Mathematically exact results when possible within Vertica’s numeric domain.
- Explicit diagnostics when precision requirements exceed those limits.
- Familiar SQL semantics (NULL handling, group aggregation) with enhanced
  safety for extreme numeric workloads.
