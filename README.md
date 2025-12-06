# exact_sum – High-Precision SUM() UDX for Vertica

---

## ⚠️ Disclaimer

This software—including the `exact_sum` UDx implementation, supporting scripts, and documentation—is provided **“as is”**, without any warranties of any kind, whether express or implied.  
No guarantees are made regarding accuracy, reliability, performance, or suitability for any particular purpose.  

Before using this code in production systems or mission-critical environments, you should:

- Validate correctness with your own datasets,  
- Perform extensive testing under expected workload conditions, and  
- Review Vertica’s documentation regarding UDx development and NUMERIC precision handling.

---


## 1. Overview

This project provides a Vertica User Defined Aggregate Function (UDAF) called `exact_sum`
for computing sums on very large `NUMERIC(p, s)` values with:

- **High precision** for extreme numeric ranges and very large row counts.
- **Dynamic precision management** for performance.
- **Clear diagnostics** when the required precision exceeds Vertica’s `NUMERIC(1024, s)` limit.

Vertica’s built-in aggregates work well for typical workloads.  
This UDX enhances precision handling for rare **extreme-value** scenarios where users require:

- additional numeric safety,
- guaranteed accuracy when possible,
- explicit errors instead of silent truncation.

`exact_sum` will either return the mathematically correct sum or clearly explain why the calculation cannot be guaranteed within Vertica’s numeric limits.

---

## 2. What `exact_sum` does

### Function Signature

```sql
exact_sum(a NUMERIC(p, s)) RETURNS NUMERIC(p_out, s_out)
```

Where:

```
p_out = min(1024, p_in + 5)
s_out = min(p_out, s_in + 5)
```

This ensures the output:

- Preserves the input scale.
- Adds a few digits for precision.
- Adapts dynamically to the input column’s numeric properties.

---

## 3. Internal Approach 

### Intermediate state contains:

- A **wide NUMERIC sum** type (`p_sum = min(1024, p_in + 19)`),
- Row count (`cnt`),
- Input precision/scale (`p_in`, `s_in`).

The UDX adds **19 digits** to internal precision because:

- `ceil(log10(N))` for any Vertica row count (`N ≤ 9e18`) is ≤ 19.

This makes the SUM precise **whenever it is mathematically representable** within Vertica’s maximum precision.

### Final Step Logic

During termination:

1. Compute `digits(rowCount)`
2. Compute required precision:

   ```
   p_needed = p_in + digits(rowCount)
   ```

3. If `p_needed > 1024`  
   → No exact SUM is possible within Vertica's numeric limits.  
   → UDX returns a **clear diagnostic error**.

4. Otherwise  
   → SUM fits exactly, division is exact, the result is mathematically correct.

---

## 4. Repository Contents

| File | Description |
|------|-------------|
| **exact_sum.cpp** | UDX implementation using Vertica SDK |
| **Makefile** | Builds `/tmp/exact_sum.so` |
| **1_compile.sh** | Wrapper script invoking `make` |
| **2_register_and_test.sql** | Registers UDX + small sample test |
| **3_stress_test.sql** | Extreme dataset test (up to 100M rows) |

---

## 5. Build Instructions
  exact_sum.cpp  Makefile_exact_sum  OLD  README_exact_sum.md
Run on the Vertica node with SDK installed:

```bash
./1_compile_exact_sum.sh
```

The Makefile prints whether build succeeded or failed.

---

## 6. Register the UDX

```bash
vsql -ef 2_register_exact_sum.sql
```

This:

1. Creates `exact_sum_lib`
2. Creates `exact_sum` aggregate
3. Grants PUBLIC access
---

## 7. Test

To test extreme numeric conditions:

```bash
vsql -ef 3_test_exact_sum.sql
```

This script:

- Creates 1000 rows of very large NUMERIC values,
- Compares:
  - `SUM(a)`
  - `EXACT_SUM(a)`
  - `exact_sum(a)`
- Computes the true mathematical sum analytically:

  ```
  BASE + (n + 1)/2
  And find smallest row count N where the built-in SUM exceeds its internal 256-bit limit is 403 rows:
  -[ RECORD 1 ]-+-------------------------------------------------------------------------------------------------
  boundary_kind | correct_until_here
  n_rows        | 402
  built_in_sum  | 578608270920987278375568558780822939733758459173977325560900545440417713141.26
  exact_sum     | 578608270920987278375568558780822939733758459173977325560900545440417713141.26
  expected_sum  | 578608270920987278375568558780822939733758459173977325560900545440417713141.26000000000000000000
  gap           | 0.00
  -[ RECORD 2 ]-+-------------------------------------------------------------------------------------------------
  boundary_kind | first_overflow
  n_rows        | 403
  built_in_sum  | -577873297395157294570649827229486927506071341564085087655663104227170255411.97
  exact_sum     | 580047594978004659665060022857392151026628505092320552738912735851961040987.39
  expected_sum  | 580047594978004659665060022857392151026628505092320552738912735851961040987.39000000000000000000
  gap           | -1157920892373161954235709850086879078532699846656405640394575840079131296399.36

  ##### ===== SUMMARY =====
  ##### The reported gap value -1157920892373161954235709850086879078532699846656405640394575840079131296399.36
  ##### is exactly -2^256 / 100 when we compute it numerically, which matches the idea that Vertica’s SUM()
  ##### for this NUMERIC(75,2) pattern is using an internal accumulator equivalent to a 256-bit integer scaled by 10².
  ##### For n_rows = 402, the true mathematical sum is still within the positive range of that accumulator,
  ##### so SUM(a) and exact_sum(a) agree and gap = 0.
  ##### When we move to n_rows = 403, the true sum crosses that internal limit, the accumulator wraps once modulo 2^256,
  ##### and the result is exactly one “wrap amount” (2²⁵⁶/100) lower than the mathematically correct value,
  ##### hence the large negative constant gap that appears for 403 and then stays constant as we keep adding rows:
  ##### built_in_sum = exact_sum - 2^256/100  → large negative decimal

  ```

- Shows that:

  ```
  exact_sum(a) - expected_sum = 0.0
  ```

---

## 8. Using exact_sum in Your Own Queries

```sql
SELECT exact_sum(a) FROM big_table;

SELECT customer_id, exact_sum(order_total)
FROM orders
GROUP BY customer_id;
```

- NULLs are ignored (standard SQL behavior).
- Returns NULL if all rows in a group are NULL.
- Provides precise results or clear diagnostics when precision is mathematically impossible.

---

## 9. Notes

- This UDX respects Vertica's global numeric limit (`NUMERIC(1024, s)`).
- It is designed for **extreme** numeric workloads, not typical queries.
- Produces either:
  - exact mathematical result, or
  - explicit explanation of why precision cannot be guaranteed.

---

## 10. Summary

`exact_sum` offers:

- Dynamic precision handling  
- Mathematical guarantees when possible  
- Transparent diagnostics when limits are exceeded  
- Performance suitable for large datasets  
- Drop-in replacement for special high-precision needs  


