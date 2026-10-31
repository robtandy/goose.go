// Selects (issue #26): std's min, max, clamp, abs and sign, a function of
// the program's own that chooses as max does, and an if choosing between
// two values, through the Go entry points of export fns. selects_test.go
// compares each with the same choice worked out in Go, on random data and on
// the ends of every type's range, NaNs and signed zeros included, and
// run_go_tests.py reads the code Go makes of each annotated one for arm64
// and for amd64 (Runner.select_asm): `asm: select, branches m` is at least
// one conditional select (CSEL and its kin on arm64, CMOV on amd64) and
// exactly m conditional branches, those of the loop and of the guard, so
// that no choice is a branch.
import std;

// ---- reductions

// The max-reduction of issue #26.
// asm: select, branches 1
export fn max_i64(xs: const i64[:]) -> i64 {
    var m = -9223372036854775807 - 1;
    for x in xs { m = max(m, x); }
    m
}
// asm: select, branches 1
export fn min_i64(xs: const i64[:]) -> i64 {
    var m = 9223372036854775807;
    for x in xs { m = min(m, x); }
    m
}
// asm: select, branches 1
export fn max_u64(xs: const u64[:]) -> u64 {
    var m: u64 = 0;
    for x in xs { m = max(m, x); }
    m
}
// asm: select, branches 1
export fn min_i32(xs: const i32[:]) -> i32 {
    var m: i32 = 2147483647;
    for x in xs { m = min(m, x); }
    m
}
// asm: select, branches 1
export fn max_u16(xs: const u16[:]) -> u16 {
    var m: u16 = 0;
    for x in xs { m = max(m, x); }
    m
}
// A byte: amd64 has no conditional move of one.
// asm: select, branches 1
export fn max_u8(xs: const u8[:]) -> u8 {
    var m: u8 = 0;
    for x in xs { m = max(m, x); }
    m
}
// asm: select, branches 1
export fn min_i8(xs: const i8[:]) -> i8 {
    var m: i8 = 127;
    for x in xs { m = min(m, x); }
    m
}
// The largest and the least at once: their difference, wrapping.
// asm: select, branches 1
export fn spread(xs: const i64[:]) -> i64 {
    var lo = 9223372036854775807;
    var hi = -9223372036854775807 - 1;
    for x in xs {
        lo = min(lo, x);
        hi = max(hi, x);
    }
    hi - lo
}
// The sum of the positive parts.
// asm: select, branches 1
export fn relu_sum(xs: const i64[:]) -> i64 {
    var s = 0;
    for x in xs { s += max(x, 0); }
    s
}
// The sum of each value clamped to [lo, hi]: two guards and a value.
// asm: select, branches 1
export fn clamp_sum(xs: const i64[:], lo: i64, hi: i64) -> i64 {
    var s = 0;
    for x in xs { s += clamp(x, lo, hi); }
    s
}

// ---- in a checkfree fn (§7.14), which may call std's choices of scalars
// under --target=go (issue #62): none leaves a bounds check in it.

// The largest of xs, each capped at hi.
// asm: select, branches 1
export checkfree fn capped_max(xs: const i64[:], hi: i64) -> i64 {
    var m = -9223372036854775807 - 1;
    for x in xs { m = max(m, min(x, hi)); }
    m
}
// The sum of each value clamped to [lo, hi], of its magnitude and its sign.
// asm: select, branches 1
export checkfree fn choices_sum(xs: const i64[:], lo: i64, hi: i64) -> i64 {
    var s = 0;
    for x in xs { s += clamp(x, lo, hi) + abs(x) + sign(x); }
    s
}

// A function of the program's own, which chooses as std's max does.
fn larger(a: i32, b: i32) -> i32 {
    if a > b { return a; }
    return b;
}
// asm: select, branches 1
export fn larger_i32(xs: const i32[:]) -> i32 {
    var m: i32 = -2147483647 - 1;
    for x in xs { m = larger(m, x); }
    m
}

// ---- each element's choice stored in place, once

// asm: select, branches 2
export fn max_into(o: i64[:], xs: const i64[:]) {
    if o.len < xs.len { return; }
    for x, i in xs { o[i] = max(o[i], x); }
}
// Each pair's max into o, which reads neither.
// asm: select, branches 3
export fn max_pairs(o: i64[:], a: const i64[:], b: const i64[:]) {
    if o.len < a.len || b.len < a.len { return; }
    for x, i in a { o[i] = max(x, b[i]); }
}
// asm: select, branches 2
export fn min_into(o: i32[:], xs: const i32[:]) {
    if o.len < xs.len { return; }
    for x, i in xs { o[i] = min(o[i], x); }
}
// Pixels clamped to [lo, hi].
// asm: select, branches 1
export fn clamp_u8(o: u8[:], lo: u8, hi: u8) {
    for k in o.len { o[k] = clamp(o[k], lo, hi); }
}
// asm: select, branches 1
export fn clamp_into(o: i64[:], lo: i64, hi: i64) {
    for k in o.len { o[k] = clamp(o[k], lo, hi); }
}
// asm: select, branches 1
export fn abs_into(o: i64[:]) {
    for k in o.len { o[k] = abs(o[k]); }
}
// asm: select, branches 1
export fn sign_into(o: i64[:]) {
    for k in o.len { o[k] = sign(o[k]); }
}
// An if choosing between two values.
// asm: select, branches 1
export fn at_least_into(o: i64[:], t: i64) {
    for k in o.len {
        let x = o[k];
        o[k] = if x < t { t } else { x };
    }
}

// ---- one choice

// asm: select, branches 0
export fn max2(a: i64, b: i64) -> i64 { max(a, b) }
// asm: select, branches 0
export fn min2_u8(a: u8, b: u8) -> u8 { min(a, b) }
// asm: select, branches 0
export fn clamp3(x: i32, lo: i32, hi: i32) -> i32 { clamp(x, lo, hi) }
// asm: select, branches 0
export fn sign1(x: i64) -> i64 { sign(x) }

// ---- floats, as std chooses: `max(a, b)` is `a < b ? b : a`, whatever a
// NaN or a zero's sign, where Go's builtin max has max(x, NaN) = NaN and
// max(-0.0, 0.0) = 0.0

export fn max_f64(a: f64, b: f64) -> f64 { max(a, b) }
export fn min_f64(a: f64, b: f64) -> f64 { min(a, b) }
export fn max_f32(a: f32, b: f32) -> f32 { max(a, b) }
export fn min_f32(a: f32, b: f32) -> f32 { min(a, b) }
export fn clamp_f64(x: f64, lo: f64, hi: f64) -> f64 { clamp(x, lo, hi) }
export fn abs_f64(x: f64) -> f64 { abs(x) }
export fn max_f64s(xs: const f64[:]) -> f64 {
    var m = if xs.len > 0 { xs[0] } else { 0.0 };
    for x in xs { m = max(m, x); }
    m
}
export fn max_f64_into(o: f64[:], xs: const f64[:]) {
    if o.len < xs.len { return; }
    for x, i in xs { o[i] = max(o[i], x); }
}
