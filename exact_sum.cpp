#include "Vertica.h"
#include <vector>
#include <exception>

using namespace Vertica;

// Maximum precision Vertica allows for NUMERIC; used as an absolute ceiling.
static const int32 MAX_NUMERIC_PRECISION = 1024;

/**
 * exact_sum(NUMERIC(p,s)) -> NUMERIC(p_out, s_out)
 *
 * Goals:
 *  - Compute a mathematically exact SUM whenever the true SUM can fit in Vertica's
 *    NUMERIC(1024, ...) domain.
 *  - Detect when an exact SUM would require more than 1024 digits of precision
 *    and raise a clear error instead of returning a silently truncated result.
 *  - Use a dynamically-sized intermediate NUMERIC type for SUM for better
 *    performance than always using NUMERIC(1024, ...).
 *
 * Theory:
 *  - Input type: NUMERIC(p_in, s_in).
 *  - Worst-case magnitude of each value: < 10^(p_in - s_in).
 *  - Sum of N such values can need up to:
 *        p_needed = p_in + ceil(log10(N))
 *    decimal digits of precision in total (integer + fractional).
 *  - Vertica caps p at 1024, so if p_needed > 1024, no exact sum is possible
 *    for any implementation (built-in or UDX).
 *
 * Implementation:
 *  - Intermediate SUM type: NUMERIC(p_sum, s_sum) with
 *        p_sum = min(1024, p_in + 19),
 *        s_sum = clamp(s_in, 0, p_sum).
 *    19 extra digits covers any possible 64-bit row count (N <= 9e18, 19 digits).
 *  - We store p_in and s_in in the intermediate state alongside sum and cnt.
 *  - In terminate():
 *        p_needed = p_in + digits10(rowCount)
 *    If p_needed > 1024, we raise a clear error that explains the problem.
 *    Otherwise, p_sum >= p_needed by construction, so the SUM we accumulated is
 *    exactly representable and we can safely return it.
 */
class ExactSum : public AggregateFunction
{
public:
    // Let Vertica generate the vectorized aggregateArrs() wrapper.
    // It will call our aggregate() below for each chunk.
    InlineAggregate();

    // Initialize intermediate state: sum = 0, cnt = 0, p_in = 0, s_in = 0
    virtual void initAggregate(ServerInterface &srvInterface,
                               IntermediateAggs &aggs)
    {
        try {
            VNumeric &sum = aggs.getNumericRef(0);
            sum.setZero();

            vint &cnt = aggs.getIntRef(1);
            cnt = 0;

            // p_in and s_in will be set in the first call to aggregate()
            // based on the input column type.
            vint &p_in_stored = aggs.getIntRef(2);
            p_in_stored = 0;

            vint &s_in_stored = aggs.getIntRef(3);
            s_in_stored = 0;
        } catch (std::exception &e) {
            vt_report_error(0,
                "exact_sum: error in initAggregate: [%s]", e.what());
        }
    }

    // Aggregate input rows into (sum, cnt)
    virtual void aggregate(ServerInterface &srvInterface,
                           BlockReader &argReader,
                           IntermediateAggs &aggs)
    {
        try {
            VNumeric &sum = aggs.getNumericRef(0);
            vint &cnt = aggs.getIntRef(1);
            vint &p_in_stored = aggs.getIntRef(2);
            vint &s_in_stored = aggs.getIntRef(3);

            // On the first call, record the input NUMERIC(p_in, s_in) in the
            // intermediate state so we can use it later to diagnose overflow.
            if (p_in_stored == 0) {
                const VerticaType &inType =
                    argReader.getTypeMetaData().getColumnType(0);

                if (!inType.isNumeric()) {
                    vt_report_error(0,
                        "exact_sum expects a NUMERIC/DECIMAL input type");
                }

                int32 p_in = inType.getNumericPrecision();
                int32 s_in = inType.getNumericScale();

                if (p_in <= 0 || p_in > MAX_NUMERIC_PRECISION) {
                    vt_report_error(0,
                        "exact_sum: invalid input NUMERIC precision %d", p_in);
                }

                p_in_stored = p_in;
                s_in_stored = s_in;
            }

            do {
                const VNumeric &input = argReader.getNumericRef(0);
                if (!input.isNull()) {
                    // sum += input (high precision NUMERIC)
                    sum.accumulate(&input);
                    // count only non-NULL rows (SQL SUM semantics: ignores NULLs,
                    // but we still need the non-NULL count for overflow analysis).
                    cnt++;
                }
            } while (argReader.next());
        } catch (std::exception &e) {
            vt_report_error(0,
                "exact_sum: error in aggregate: [%s]", e.what());
        }
    }

    // Combine partial aggregates (sum, cnt, p_in, s_in) from different nodes/threads
    virtual void combine(ServerInterface &srvInterface,
                         IntermediateAggs &aggs,
                         MultipleIntermediateAggs &aggsOther)
    {
        try {
            VNumeric &mySum = aggs.getNumericRef(0);
            vint &myCnt = aggs.getIntRef(1);
            vint &myPIn = aggs.getIntRef(2);
            vint &mySIn = aggs.getIntRef(3);

            do {
                const VNumeric &otherSum = aggsOther.getNumericRef(0);
                const vint &otherCnt = aggsOther.getIntRef(1);
                const vint &otherPIn = aggsOther.getIntRef(2);
                const vint &otherSIn = aggsOther.getIntRef(3);

                mySum.accumulate(&otherSum);
                myCnt += otherCnt;

                // p_in and s_in are properties of the input column type, so
                // they should match across all partials. For robustness, we
                // take the maximum we see (they should all be equal in practice).
                if (otherPIn > myPIn) {
                    myPIn = otherPIn;
                }
                if (otherSIn > mySIn) {
                    mySIn = otherSIn;
                }
            } while (aggsOther.next());
        } catch (std::exception &e) {
            vt_report_error(0,
                "exact_sum: error in combine: [%s]", e.what());
        }
    }

    // Finalize: return SUM with overflow diagnosis.
    virtual void terminate(ServerInterface &srvInterface,
                           BlockWriter &resWriter,
                           IntermediateAggs &aggs)
    {
        try {
            const VNumeric &sum = aggs.getNumericRef(0);
            const vint &rowCount = aggs.getIntRef(1);
            const vint &p_in_stored = aggs.getIntRef(2);
            const vint &s_in_stored = aggs.getIntRef(3);

            VNumeric &out = resWriter.getNumericRef(0);

            // No non-NULL rows in this group → return NULL (like SUM)
            if (rowCount == 0) {
                out.setNull();
                return;
            }

            // Dummy usage of s_in_stored to avoid unused-variable warnings
            // without affecting accuracy or logic.
            if (s_in_stored < 0) {
                vt_report_error(0,
                    "exact_sum: internal error: invalid stored input scale %lld",
                    static_cast<long long>(s_in_stored));
            }

            // Sanity check: we must know input precision to reason about overflow.
            if (p_in_stored <= 0 || p_in_stored > MAX_NUMERIC_PRECISION) {
                vt_report_error(0,
                    "exact_sum: internal error: invalid stored input precision %lld",
                    static_cast<long long>(p_in_stored));
            }

            // Compute the number of decimal digits needed to represent rowCount.
            // For example:
            //   rowCount = 1        -> digitsN = 1
            //   rowCount = 10       -> digitsN = 2
            //   rowCount = 12345    -> digitsN = 5
            if (rowCount < 0) {
                vt_report_error(0,
                    "exact_sum: internal error: negative row count %lld",
                    static_cast<long long>(rowCount));
            }

            vint tmp = rowCount;
            int32 digitsN = 0;
            while (tmp > 0) {
                tmp /= 10;
                digitsN++;
            }
            if (digitsN == 0) {
                // This can only happen if rowCount == 0, which is already handled,
                // but we guard for completeness.
                vt_report_error(0,
                    "exact_sum: internal error: zero digit count for row count %lld",
                    static_cast<long long>(rowCount));
            }

            // Worst-case total precision needed for the SUM:
            //   p_needed = p_in + ceil(log10(rowCount)) = p_in + digitsN
            int32 p_in = static_cast<int32>(p_in_stored);
            int32 p_needed = p_in + digitsN;

            // If the required precision exceeds Vertica's absolute cap (1024),
            // no implementation can compute an exact SUM; we must fail loudly.
            if (p_needed > MAX_NUMERIC_PRECISION) {
                vt_report_error(
                    0,
                    "exact_sum: Cannot calculate the exact sum for such huge numbers: "
                    "required precision %d (input precision %d plus %d digits for row count %lld) "
                    "exceeds Vertica NUMERIC maximum precision %d. "
                    "Consider reducing the magnitude or number of rows.",
                    p_needed,
                    p_in,
                    digitsN,
                    static_cast<long long>(rowCount),
                    MAX_NUMERIC_PRECISION);
            }

            // At this point, we know:
            //   - p_needed <= 1024, so the exact sum CAN be represented.
            //   - In getIntermediateTypes(), we chose p_sum = min(1024, p_in + 19).
            //   - digitsN <= 19 for any 64-bit rowCount.
            //   Therefore p_sum >= p_in + digitsN = p_needed, so the SUM we
            //   accumulated is exactly representable in our intermediate type.
            //
            // Now we simply copy that exact SUM into the output.
            out.setZero();
            out.accumulate(&sum);
        } catch (std::exception &e) {
            vt_report_error(
                0,
                "exact_sum: error in terminate (overflow diagnostics): [%s]",
                e.what());
        }
    }
};


/**
 * Factory: validates arguments, chooses return type, and defines
 * intermediate (sum, cnt, p_in, s_in) types.
 */
class ExactSumFactory : public AggregateFunctionFactory
{
public:
    // One NUMERIC argument, numeric return
    virtual void getPrototype(ServerInterface &srvInterface,
                              ColumnTypes &argTypes,
                              ColumnTypes &returnType)
    {
        argTypes.addNumeric();   // input must be NUMERIC/DECIMAL
        returnType.addNumeric(); // actual p,s decided in getReturnType()
    }

    // Decide output NUMERIC(p_out, s_out) based on input NUMERIC(p_in, s_in)
    virtual void getReturnType(ServerInterface &srvInterface,
                               const SizedColumnTypes &inputTypes,
                               SizedColumnTypes &outputTypes)
    {
        if (inputTypes.getColumnCount() != 1) {
            vt_report_error(0,
                "exact_sum expects exactly one argument");
        }

        const VerticaType &inType = inputTypes.getColumnType(0);

        if (!inType.isNumeric()) {
            vt_report_error(0,
                "exact_sum expects a NUMERIC/DECIMAL input type");
        }

        int32 p_in = inType.getNumericPrecision();
        int32 s_in = inType.getNumericScale();

        if (p_in <= 0 || p_in > MAX_NUMERIC_PRECISION) {
            vt_report_error(0,
                "exact_sum: invalid input NUMERIC precision %d", p_in);
        }

        /*
         * For the SUM, worst-case required precision is:
         *
         *   p_needed = p_in + ceil(log10(N))
         *
         * For any 64-bit row count, ceil(log10(N)) <= 19, so adding 19 digits
         * is always enough when an exact SUM is representable within Vertica's
         * NUMERIC(1024, ...) limit.
         *
         * We therefore choose:
         *
         *   p_out = min(1024, p_in + 19)
         *   s_out = clamp(s_in, 0, p_out)
         *
         * which guarantees that, whenever p_needed <= 1024, the final SUM can
         * be represented exactly in the output type.
         */
        int32 extra_digits_for_rows = 19;
        int32 p_out = p_in + extra_digits_for_rows;
        if (p_out > MAX_NUMERIC_PRECISION) {
            p_out = MAX_NUMERIC_PRECISION;
        }

        int32 s_out = s_in;
        if (s_out > p_out) {
            s_out = p_out;   // scale cannot exceed precision
        }
        if (s_out < 0) {
            s_out = 0;
        }

        outputTypes.addNumeric(p_out, s_out, "exact_sum");
    }

    // Decide intermediate (sum, cnt, p_in, s_in) types
    virtual void getIntermediateTypes(ServerInterface &srvInterface,
                                      const SizedColumnTypes &inputTypes,
                                      SizedColumnTypes &intermediateTypes)
    {
        if (inputTypes.getColumnCount() != 1) {
            vt_report_error(0,
                "exact_sum expects exactly one argument");
        }

        const VerticaType &inType = inputTypes.getColumnType(0);

        if (!inType.isNumeric()) {
            vt_report_error(0,
                "exact_sum expects a NUMERIC/DECIMAL input type");
        }

        int32 p_in = inType.getNumericPrecision();
        int32 s_in = inType.getNumericScale();

        if (p_in <= 0 || p_in > MAX_NUMERIC_PRECISION) {
            vt_report_error(0,
                "exact_sum: invalid input NUMERIC precision %d", p_in);
        }

        /*
         * Performance vs safety for the SUM precision:
         *
         *   - Worst-case we need: p_needed = p_in + ceil(log10(N))
         *   - For a 64-bit row count, N <= 9,223,372,036,854,775,807, so
         *         ceil(log10(N)) <= 19.
         *   - So p_in + 19 digits are enough to represent the SUM exactly
         *     for any possible rowCount within Vertica.
         *
         * We choose:
         *   p_sum = min(1024, p_in + 19)
         *
         * This is:
         *   - Always large enough when an exact sum is representable
         *     (p_needed <= 1024).
         *   - Cheaper than always using p_sum = 1024 for small/moderate p_in.
         */
        int32 extra_digits_for_rows = 19;
        int32 p_sum = p_in + extra_digits_for_rows;
        if (p_sum > MAX_NUMERIC_PRECISION) {
            p_sum = MAX_NUMERIC_PRECISION;
        }

        // Keep the same scale for the sum as the input, clamped to [0, p_sum].
        int32 s_sum = s_in;
        if (s_sum > p_sum) {
            s_sum = p_sum;
        }
        if (s_sum < 0) {
            s_sum = 0;
        }

        intermediateTypes.addNumeric(p_sum, s_sum, "sum"); // index 0
        intermediateTypes.addInt("cnt");                   // index 1
        intermediateTypes.addInt("p_in");                  // index 2
        intermediateTypes.addInt("s_in");                  // index 3
    }

    virtual AggregateFunction *createAggregateFunction(
        ServerInterface &srvInterface)
    {
        return vt_createFuncObject<ExactSum>(srvInterface.allocator);
    }
};

RegisterFactory(ExactSumFactory);
