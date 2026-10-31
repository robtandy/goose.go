// Constant tables of a library (issue #62), compiled with --target=go
// --library. A global the C backend makes static data -- a `const`, or a
// `let` of a const type, whose initializer is literals -- is Go static data
// too where Go can spell its value as a constant: a table of integers, of
// floats, of bools, of a string's bytes, of native structs, a fill, a table
// of tables. Go's linker lays each out with its value, so that it holds it
// before anything runs, the library's start included, which has none of
// them to initialize. A global the start computes is the start's still, and
// every entry point of an export fn whose calls read one starts the
// library, a kernel's that takes no block included. static_tables_test.go
// checks all of that from its TestMain on, before any call.

// The constants of a 64-bit FNV-1a and of a multiplicative mix.
const KEY: u64[8] = [14695981039346656037, 1099511628211, 0x9e3779b97f4a7c15, 0xbf58476d1ce4e5b9,
                     0x94d049bb133111eb, 1, 0xffffffffffffffff, 0];
// A byte's substitution, and its inverse's first entries.
const SBOX: u8[16] = [12, 5, 6, 11, 9, 0, 10, 13, 3, 14, 15, 8, 4, 7, 1, 2];
let INV: const u8[4] = [5, 14, 15, 8];
// Signed values at their types' ends.
const ENDS: i32[4] = [-2147483648, -1, 0, 2147483647];
const WIDE: i64[2] = [-9223372036854775807 - 1, 9223372036854775807];
// A string's bytes.
const HELLO: u8[5] = "hello";
// Floats Go has constants for.
const HALVES: f64[3] = [0.5, -2.25, 1e300];
const THIRDS: f32[2] = [0.33333334, -1.5];
const FLAGS: bool[3] = [true, false, true];
// A fill.
const ONES: u16[8] = [1; 8];
// A table of tables.
const GRID: i16[3][2] = [[1, -2, 3], [-4, 5, -6]];
// Native structs, alone and in a table.
struct Span { lo: i32, hi: i32 }
const UNIT: Span = Span { lo: 0, hi: 1 };
const SPANS: Span[3] = [Span { lo: 0, hi: 9 }, Span { lo: 10, hi: 99 }, Span { lo: 100, hi: 999 }];
// One named by another.
const KEY2: u64[8] = KEY;

// A negative zero, which Go has no constant for: the start's.
const ZEROS: f64[2] = [0.0, -0.0];
// A global the start computes, from the tables.
fn seed() -> u64 { KEY[2] ^ KEY[3] ^ SBOX[15] }
let SEED = seed();

// FNV-1a of a word's bytes, from the table alone.
export fn fnv(x: u64) -> u64 {
    var h = KEY[0];
    for i in 8 { h = (h ^ ((x >> (8 * i)) & 255)) * KEY[1]; }
    h
}

// The substitution of each nibble of x.
export fn substitute(x: u8) -> u8 { SBOX[x >> 4] << 4 | SBOX[x & 15] }

// The digits of x up to 999, by the spans: 1, 2 or 3; 0 past them.
export fn digits(x: i32) -> i64 {
    for s, i in SPANS {
        if x >= s.lo && x <= s.hi { return i + 1; }
    }
    0
}

// A sum over the other tables, each read once.
export fn tables_sum() -> i64 {
    var s = 0;
    for x in ENDS { s += x; }
    for x in WIDE { s += x; }
    for x in HELLO { s += x; }
    for x in ONES { s += x; }
    for row in GRID {
        for x in row { s += x; }
    }
    for x in INV { s += x; }
    for b in FLAGS {
        if b { s += 1000; }
    }
    s + UNIT.hi + KEY2[1] as! i64
}

// The floats' sum.
export fn floats_sum() -> f64 { HALVES[0] + HALVES[1] + THIRDS[0] + THIRDS[1] + ZEROS[1] }

// Whether ZEROS[1] is the negative zero, which the start leaves there.
export fn negative_zero() -> bool { 1.0 / ZEROS[1] < 0.0 }

// The mix of x with the seed the start computes: a kernel, whose entry
// points take no block, calling a function that reads the global.
fn mix(x: u64) -> u64 { (x ^ SEED) * KEY[4] }
export fn mixed(x: u64) -> u64 { mix(x) }
