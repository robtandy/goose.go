// A module of selects.goose and selects_debug.goose (issue #26): bodies that
// only choose among values they compute nothing for -- std's min, max,
// clamp, abs and sign, functions of the program's own written as guards, as
// an else-if chain or as an if's two returns, and ifs choosing between two
// values -- which the Go backend writes as Go's builtin min and max or as
// assignments of a temporary that Go makes conditional selects of
// (src/codegen_go_select.h). Each is checked at every scalar type, at the
// ends of its range, with NaNs and signed zeros, into every kind of
// destination (a local, an element, a field, a global, a reference's
// pointee, a narrower or a wider slot), in expressions, conditions and
// arguments, and next to what must stay a branch: a choice that calls, or
// a negation a debug build checks, which only the branch taken may run.
import std;

// ---- the values at the ends of each type's range

fn i64s() -> i64[4] { [-9223372036854775807 - 1, -1, 0, 9223372036854775807] }
fn u64s() -> u64[3] { [0, 1, 18446744073709551615] }
fn i32s() -> i32[4] { [-2147483647 - 1, -7, 0, 2147483647] }
fn u32s() -> u32[3] { [0, 5, 4294967295] }
fn i16s() -> i16[4] { [-32768, -1, 1, 32767] }
fn u16s() -> u16[3] { [0, 300, 65535] }
fn i8s() -> i8[4] { [-128, -1, 0, 127] }
fn u8s() -> u8[4] { [0, 1, 200, 255] }

fn pairs_i64() {
    let v = i64s();
    var line: u8[>..] = [];
    for a in v {
        for b in v { line.format(min(a, b), "/", max(a, b), " "); }
    }
    print("i64 ", line);
}

fn pairs_narrow() {
    var line: u8[>..] = [];
    let a64 = u64s();
    for a in a64 { for b in a64 { line.format(min(a, b), "/", max(a, b), " "); } }
    print("u64 ", line);
    line.clear();
    let a32 = i32s();
    for a in a32 { for b in a32 { line.format(min(a, b), "/", max(a, b), " "); } }
    print("i32 ", line);
    line.clear();
    let b32 = u32s();
    for a in b32 { for b in b32 { line.format(min(a, b), "/", max(a, b), " "); } }
    print("u32 ", line);
    line.clear();
    let a16 = i16s();
    for a in a16 { for b in a16 { line.format(min(a, b), "/", max(a, b), " "); } }
    print("i16 ", line);
    line.clear();
    let b16 = u16s();
    for a in b16 { for b in b16 { line.format(min(a, b), "/", max(a, b), " "); } }
    print("u16 ", line);
    line.clear();
    let a8 = i8s();
    for a in a8 { for b in a8 { line.format(min(a, b), "/", max(a, b), " "); } }
    print("i8 ", line);
    line.clear();
    let b8 = u8s();
    for a in b8 { for b in b8 { line.format(min(a, b), "/", max(a, b), " "); } }
    print("u8 ", line);
}

// clamp at its bounds, inside them, past them, and with the bounds crossed.
fn clamps() {
    var line: u8[>..] = [];
    for x in -3..4 {
        line.format(clamp(x, -1, 1), ",", clamp(x, 1, -1), ",", clamp(x, 2, 2), " ");
    }
    print("clamp i64 ", line);
    line.clear();
    let b8 = u8s();
    for x in b8 { line.format(clamp(x, 16, 235), ",", clamp(x, 235, 16), " "); }
    print("clamp u8 ", line);
    line.clear();
    let a8 = i8s();
    for x in a8 { line.format(clamp(x, -100, 100), ",", clamp(x, 0, 0), " "); }
    print("clamp i8 ", line);
}

// abs and sign, which is -1, 0 or 1, at the ends of the range but the
// least value, whose negation a debug build checks.
fn abs_signs() {
    var line: u8[>..] = [];
    let v = i64s();
    for x in v { line.format(abs(max(x, -9223372036854775807)), ",", sign(x), " "); }
    let w = i8s();
    for x in w { line.format(abs(x / 2), ",", sign(x), " "); }
    print("abs/sign ", line);
}

// ---- floats: std's choices, which are not Go's builtins'

fn floats() {
    let zero = 0.0;
    let negz = -zero;
    let nan = zero / zero;
    let inf = 1.0 / zero;
    let v = [negz, zero, 1.5, -inf, inf, nan];
    var line: u8[>..] = [];
    for a in v {
        for b in v {
            let mx = max(a, b);
            let mn = min(a, b);
            // A zero's sign shows in its reciprocal.
            line.format(mx, " ", 1.0 / mx, "/", mn, " ", 1.0 / mn, " ");
        }
    }
    print("f64 ", line);
    line.clear();
    let f: f32[4] = [-1.0, 0.0, 2.5, 3.0];
    for a in f { for b in f { line.format(max(a, b) - min(a, b), " "); } }
    for x in v { line.format(clamp(x, -1.0, 1.0), ",", abs(x), " "); }
    print("f32/clamp/abs ", line);
}

// ---- chains of the program's own

// Guards: each a guard's else returning.
fn band(x: i64) -> i64 {
    guard x >= 10 else { return 10; }
    guard x <= 20 else { return 20; }
    x
}
// An else-if chain of values.
fn grade(x: i32) -> i32 {
    if x >= 90 { 4 } else if x >= 75 { 3 } else if x >= 50 { 2 } else { 1 }
}
// An if's two returns, in the other order of the operands.
fn least(a: u16, b: u16) -> u16 {
    if a <= b { return a; } else { return b; }
}
fn most(a: i8, b: i8) -> i8 {
    if b >= a { return b; }
    return a;
}
// Not min or max: equality, and a constant chosen.
fn same_or(a: i64, b: i64, d: i64) -> i64 {
    if a == b { return d; }
    if a != d { return a; }
    b
}
// A bool chosen.
fn inside(x: i64, lo: i64, hi: i64) -> bool {
    if x < lo { return false; }
    if x > hi { return false; }
    true
}
// The negation is computed only where it is chosen: in a debug build it
// fails at the least value, which this chain never negates.
fn neg_if(x: i64, c: bool) -> i64 {
    if c { return -x; }
    return x;
}
// A choice whose value calls: a branch, which runs the call only where it
// is taken.
var calls = 0;
fn counted(x: i64) -> i64 { calls += 1; x }
fn lazy_max(a: i64, b: i64) -> i64 {
    if a < b { return counted(b); }
    return a;
}

fn chains() {
    var line: u8[>..] = [];
    for x in [5, 10, 15, 20, 25] { line.format(band(x), " "); }
    let g: i32[4] = [95, 80, 60, 10];
    for x in g { line.format(grade(x), " "); }
    line.format(least(3, 9), least(9, 3), least(4, 4), " ", most(-3, 9), most(9, -3), " ");
    line.format(same_or(1, 1, 7), same_or(1, 2, 1), same_or(1, 2, 3), " ");
    line.format(inside(5, 1, 9), inside(0, 1, 9), inside(10, 1, 9), " ");
    line.format(neg_if(-9223372036854775807 - 1, false), " ", neg_if(5, true), " ");
    let m = lazy_max(1, 2) + lazy_max(5, 3);
    line.format(m, " calls ", calls);
    print("chains ", line);
}

// ---- destinations and uses

struct Range { lo: i64, hi: i64 }
var gmax = 0;

fn widen(r: Range&, x: i64) {
    r.lo = min(r.lo, x);
    r.hi = max(r.hi, x);
}

fn bump(x: i64&, y: i64) { x = max(x, y); }

var order: i64[>..] = [];
fn note(v: i64) -> i64 { order.push(v); v }

fn uses() {
    var line: u8[>..] = [];
    let xs = [4, -2, 9, 0, 7, -5];
    // A local, in a loop, read by the choice.
    var m = 0;
    for x in xs { m = clamp(max(m, x) - 1, -1, 6); }
    line.format(m, " ");
    // Elements, in place.
    var o = [1, 2, 3, 4, 5, 6];
    for x, i in xs { o[i] = max(o[i], x); }
    for x, i in xs { o[i] = if o[i] < 5 { x } else { o[i] }; }
    for v in o { line.format(v, ","); }
    // Fields of a local struct, through a reference, and in an array.
    var r = Range { lo: 0, hi: 0 };
    for x in xs { widen(r, x); }
    var rs = [Range { lo: 0, hi: 0 }, Range { lo: 0, hi: 0 }];
    for x, i in xs { rs[i % 2].hi = max(rs[i % 2].hi, x); }
    line.format(" ", r.lo, "..", r.hi, " ", rs[0].hi, ",", rs[1].hi, " ");
    // A global, and a reference's pointee.
    for x in xs { gmax = max(gmax, x * 2); }
    var b = 3;
    bump(b, 8);
    bump(b, 1);
    line.format(gmax, " ", b, " ");
    // In expressions, conditions and arguments; a wider slot.
    let a: i32 = 7;
    let c: i32 = -3;
    let w: i64 = max(a, c);
    let s = max(a, c) + min(a, c) * 2;
    if max(a, c) > 5 && min(a, c) < 0 { line.format("cond "); }
    line.format(w, " ", s, " ", max(max(1, 2), max(3, min(4, 0))), " ");
    // Arguments with effects, evaluated in order, once each.
    let e = max(note(3), note(1)) + min(note(2), note(5));
    line.format(e, " order");
    for v in order { line.format(" ", v); }
    print("uses ", line);
}

// A choice whose argument ends the function: nothing after it runs.
fn early(x: i64) -> i64 {
    let m = max(x, if x < 0 { return -1; } else { x * 2 });
    m + 1
}

fn reductions() {
    var line: u8[>..] = [];
    var xs: i64[>..] = [];
    var seed: u64 = 26;
    for i in 64 {
        seed = seed * 6364136223846793005 + 1442695040888963407;
        xs.push((seed >> 33) as! i64 - 1073741824 + i);
    }
    var lo = 9223372036854775807;
    var hi = -9223372036854775807 - 1;
    var s = 0;
    for x in xs {
        lo = min(lo, x);
        hi = max(hi, x);
        s += clamp(x, -1000000, 1000000) + max(x, 0) % 7;
    }
    var bytes: u8[>..] = [];
    for x in xs { bytes.push(x as! u8); }
    var bm: u8 = 0;
    var bn: u8 = 255;
    for x in bytes { bm = max(bm, x); bn = min(bn, x); }
    for i in bytes.len { bytes[i] = clamp(bytes[i], 40, 200); }
    var bs = 0;
    for x in bytes { bs += x; }
    line.format(lo, " ", hi, " ", s, " ", bm, " ", bn, " ", bs, " ", early(-4), " ", early(4));
    print("reductions ", line);
}

fn selects_run() {
    pairs_i64();
    pairs_narrow();
    clamps();
    abs_signs();
    floats();
    chains();
    uses();
    reductions();
}
