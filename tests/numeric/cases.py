"""Independent cases for explicit numeric conversions.

`run_conversions.py` writes each source into a temporary project. Static cases
must be rejected at check time with the bound diagnostic code and must neither
create a fresh output nor change stale output bytes. Panic cases are dynamic by
construction (a function parameter): check must accept them, and a real build
and execution must terminate with Panic exit 101 and the backend's marker.

Expected values are not repeated here; the executable value table lives in
`tests/numeric/values` and every expectation there is an exactly representable
hand-written constant.
"""


def static_cases():
    yield dict(id='static-i8-truncated-overflow',
               source='func main() i32 { let x = i8(128.5); return 0; }',
               code='E_TYPE_MISMATCH',
               reason='128.5 truncates to 128, outside i8')
    yield dict(id='static-i8-negative-overflow',
               source='func main() i32 { let x = i8(-129.0); return 0; }',
               code='E_TYPE_MISMATCH',
               reason='-129.0 truncates to -129, below i8 minimum')
    yield dict(id='static-u8-negative',
               source='func main() i32 { let x = u8(-1.0); return 0; }',
               code='E_TYPE_MISMATCH',
               reason='-1.0 truncates to -1, not representable as unsigned')
    yield dict(id='static-i64-rounded-upper',
               source='func main() i32 { let x = i64(9223372036854775807.0); return 0; }',
               code='E_TYPE_MISMATCH',
               reason='the f64 value is already 2^63, not INT64_MAX')
    yield dict(id='static-u64-rounded-upper',
               source='func main() i32 { let x = u64(18446744073709551615.0); return 0; }',
               code='E_TYPE_MISMATCH',
               reason='the f64 value is already 2^64, not UINT64_MAX')
    yield dict(id='static-nan-division',
               source='func main() i32 { let x = i32(0.0 / 0.0); return 0; }',
               code='E_TYPE_MISMATCH',
               reason='0.0/0.0 is a NaN constant')
    yield dict(id='static-nan-parenthesized',
               source='func main() i32 { let x = i32((0.0 / 0.0)); return 0; }',
               code='E_TYPE_MISMATCH',
               reason='parentheses keep the known NaN value')
    yield dict(id='static-nan-unary',
               source='func main() i32 { let x = i32(-(0.0 / 0.0)); return 0; }',
               code='E_TYPE_MISMATCH',
               reason='unary minus of a known NaN is still NaN')
    yield dict(id='static-infinity',
               source='func main() i32 { let x = i32(1.0 / 0.0); return 0; }',
               code='E_TYPE_MISMATCH',
               reason='positive infinity has no integer value')
    yield dict(id='static-negative-infinity',
               source='func main() i32 { let x = u32(-1.0 / 0.0); return 0; }',
               code='E_TYPE_MISMATCH',
               reason='negative infinity has no unsigned value')
    yield dict(id='static-nan-arithmetic',
               source='func main() i32 { let x = i32(0.0 / 0.0 + 1.0); return 0; }',
               code='E_TYPE_MISMATCH',
               reason='NaN propagates through constant float addition')
    yield dict(id='static-overflow-arithmetic',
               source='func main() i32 { let x = i32(1e308 + 1e308); return 0; }',
               code='E_TYPE_MISMATCH',
               reason='f64 addition overflows to infinity before conversion')
    yield dict(id='static-f32-nan-constant',
               source='const z f32 = 0.0; func main() i32 { let x = i32(z / z); return 0; }',
               code='E_TYPE_MISMATCH',
               reason='an f32 const keeps its own precision and NaN result')
    yield dict(id='static-negative-zero-division',
               source='func f() { let x = i32(1.0 / (-0.0 + -0.0)); return; }',
               code='E_TYPE_MISMATCH',
               reason='-0.0 + -0.0 keeps the negative zero sign, so the division is -infinity')
    yield dict(id='static-f32-overflow-constant',
               source='const big f32 = 1e38 * 10.0; func main() i32 { let x = i32(big); return 0; }',
               code='E_TYPE_MISMATCH',
               reason='f32 arithmetic overflows to infinity at f32 precision')
    yield dict(id='static-nested-nan',
               source='func main() i32 { let x = i8(i64(0.0 / 0.0)); return 0; }',
               code='E_TYPE_MISMATCH',
               reason='the inner float-to-integer conversion is already a source error')
    yield dict(id='static-nested-int-to-float',
               source='func main() i32 { let x = i8(f64(256)); return 0; }',
               code='E_TYPE_MISMATCH',
               reason='int->f64 is exact here; the outer i8 range check fails')
    yield dict(id='static-const-nan',
               source='const bad f64 = 0.0 / 0.0; func main() i32 { let x = i32(bad); return 0; }',
               code='E_TYPE_MISMATCH',
               reason='a const NaN is rejected when it reaches a conversion')
    yield dict(id='static-const-arithmetic-overflow',
               source='const bad f64 = 1e308 * 10.0; func main() i32 { let x = i32(bad); return 0; }',
               code='E_TYPE_MISMATCH',
               reason='a const infinity is rejected when it reaches a conversion')
    yield dict(id='static-bool-operand',
               source='func main() i32 { let x = i32(true); return 0; }',
               code='E_TYPE_MISMATCH',
               reason='bool is not a numeric conversion source')
    yield dict(id='unsupported-float-to-float',
               source='func main() i32 { let x = f32(1.0); return 0; }',
               code='E_CAPABILITY_UNAVAILABLE',
               reason='f64->f32 narrowing is still pending the overflow rule')
    yield dict(id='unsupported-float-to-float-dynamic',
               source='func narrow(x f64) f32 { return f32(x); } func main() i32 { return 0; }',
               code='E_CAPABILITY_UNAVAILABLE',
               reason='dynamic f64->f32 narrowing is still pending the overflow rule')
    yield dict(id='unsupported-bool-target',
               source='func main() i32 { let x = bool(1); return 0; }',
               code='E_CAPABILITY_UNAVAILABLE',
               reason='bool is not a conversion destination')
    yield dict(id='static-conversion-label',
               source='func main() i32 { let x = i32(value: 1); return 0; }',
               code='E_CALL_ARGUMENT',
               reason='explicit conversion accepts no argument labels')
    yield dict(id='static-conversion-arity',
               source='func main() i32 { let x = i32(); return 0; }',
               code='E_CALL_ARGUMENT',
               reason='explicit conversion requires exactly one argument')


def panic_cases():
    yield dict(id='dynamic-f32-nan',
               source='func narrow(x f32) i32 { return i32(x / x); } '
                      'func main() i32 { return narrow(0.0); }',
               native='integer-conversion', python='float to integer conversion of NaN',
               reason='f32 NaN reaches the dynamic conversion')
    yield dict(id='dynamic-f64-nan',
               source='func narrow(x f64) i32 { return i32(x / x); } '
                      'func main() i32 { return narrow(0.0); }',
               native='integer-conversion', python='float to integer conversion of NaN',
               reason='f64 NaN reaches the dynamic conversion')
    yield dict(id='dynamic-f64-infinity',
               source='func narrow(x f64) u64 { return u64(1.0 / x); } '
                      'func main() i32 { let v = narrow(0.0); return 0; }',
               native='integer-conversion', python='float to integer conversion of NaN or infinity',
               reason='positive infinity reaches u64')
    yield dict(id='dynamic-f32-negative-infinity',
               source='func narrow(x f32) i32 { return i32(-1.0 / x); } '
                      'func main() i32 { return narrow(0.0); }',
               native='integer-conversion', python='float to integer conversion of NaN or infinity',
               reason='negative infinity reaches i32')
    yield dict(id='dynamic-i64-upper',
               source='func narrow(x f64) i64 { return i64(x); } '
                      'func main() i32 { let v = narrow(9223372036854775808.0); return 0; }',
               native='integer-conversion', python='float to integer conversion overflow',
               reason='2^63 is one past the i64 range')
    yield dict(id='dynamic-i64-lower',
               source='func narrow(x f64) i64 { return i64(x); } '
                      'func main() i32 { let v = narrow(-9223372036854777856.0); return 0; }',
               native='integer-conversion', python='float to integer conversion overflow',
               reason='the next f64 below -2^63 truncates out of range')
    yield dict(id='dynamic-u64-upper',
               source='func narrow(x f64) u64 { return u64(x); } '
                      'func main() i32 { let v = narrow(18446744073709551616.0); return 0; }',
               native='integer-conversion', python='float to integer conversion overflow',
               reason='2^64 is one past the u64 range')
    yield dict(id='dynamic-negative-zero-infinity',
               source='func narrow(z f64) i32 { return i32(1.0 / (z + z)); } '
                      'func main() i32 { return narrow(-0.0); }',
               native='integer-conversion', python='float to integer conversion of NaN or infinity',
               reason='-0.0 + -0.0 is -0.0, so the quotient is negative infinity')
    yield dict(id='dynamic-u64-negative',
               source='func narrow(x f64) u64 { return u64(x); } '
                      'func main() i32 { let v = narrow(-1.0); return 0; }',
               native='integer-conversion', python='float to integer conversion overflow',
               reason='exactly -1.0 truncates to an invalid unsigned value')
    yield dict(id='dynamic-i8-narrow',
               source='func narrow(x f64) i8 { return i8(x); } '
                      'func main() i32 { return i32(narrow(200.0)); }',
               native='integer-conversion', python='float to integer conversion overflow',
               reason='200.0 fits i64 but not i8')
    yield dict(id='dynamic-u8-narrow',
               source='func narrow(x f32) u8 { return u8(x); } '
                      'func main() i32 { return i32(narrow(300.0)); }',
               native='integer-conversion', python='float to integer conversion overflow',
               reason='300.0 fits i64 but not u8')
    yield dict(id='dynamic-u32-narrow',
               source='func narrow(x f64) u32 { return u32(x); } '
                      'func main() i32 { return i32(narrow(4294967296.0)); }',
               native='integer-conversion', python='float to integer conversion overflow',
               reason='2^32 is one past the u32 range')
