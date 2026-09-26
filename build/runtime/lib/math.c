// The `math` standard library module (import 'math'), linked only when a
// program imports it. Signatures must match mathCallType in the checker and
// genMathCall in codegen. Every function here is one libm call: the wrapper
// keeps the ABI and the -lm dependency in one file, and LTO inlines it.
//
// No function raises and none stops the program, except rt_math_to_int. A
// float operation with no answer gives what IEEE 754 gives: NaN, or an
// infinity. abs, min, max and clamp are not here, because they are generic
// over the number types and codegen emits them inline.

#include "runtime.h"

#include <math.h>

double rt_math_sqrt(double x) { return sqrt(x); }
double rt_math_pow(double x, double y) { return pow(x, y); }
double rt_math_sin(double x) { return sin(x); }
double rt_math_cos(double x) { return cos(x); }
double rt_math_tan(double x) { return tan(x); }
double rt_math_asin(double x) { return asin(x); }
double rt_math_acos(double x) { return acos(x); }
double rt_math_atan(double x) { return atan(x); }
double rt_math_atan2(double y, double x) { return atan2(y, x); }
double rt_math_log(double x) { return log(x); }
double rt_math_log2(double x) { return log2(x); }
double rt_math_log10(double x) { return log10(x); }
double rt_math_exp(double x) { return exp(x); }
double rt_math_floor(double x) { return floor(x); }
double rt_math_ceil(double x) { return ceil(x); }
// Half away from zero, as C's round.
double rt_math_round(double x) { return round(x); }

int64_t rt_math_is_nan(double x) { return isnan(x) ? 1 : 0; }
int64_t rt_math_is_inf(double x) { return isinf(x) ? 1 : 0; }

// Truncates toward zero. A value with no int in it stops the program: the C
// conversion is undefined there, and the caller had floor, ceil and round to
// name the rounding it wanted. The bounds are exact: 2^63 is a double, and
// every double below it truncates to a value an int64 holds.
int64_t rt_math_to_int(double x) {
    if (isnan(x) || x >= 9223372036854775808.0 || x < -9223372036854775808.0) {
        rt_panic("math.toInt: the value is NaN or outside the range of an int");
    }
    return (int64_t)x;
}
