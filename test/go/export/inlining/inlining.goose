// Pure kernels over slices and their Go entry points (issue #22): export fns
// whose code takes no block of data stacks and keeps what it is handed only
// where Go sees it, in its own variables. Their entry points hand the
// arguments on as they are, with no GsEscape (which moves a caller's array
// off its stack) and no runtime.KeepAlive, so that Go inlines a call of one
// into its caller as the call of its function, which it inlines too where
// that is small. The export fns after them keep what they are handed where
// Go does not see it, or may: on a data stack, in a struct whose Go value is
// its bytes, or in the Go of an extern fn; their entry points still move the
// arguments off a caller's stack.
//
// test/run_go_tests.py reads what Go makes of each export fn an `// entry:`
// line annotates (go test -c -gcflags=-m=2, Runner.entry_points): `inline`,
// Go can inline every one of its entry points; `no escape`, none of them lets
// a slice or reference parameter escape (`does not escape`, or leaks it to
// its result only); `escape`, each does (`leaking param`); `block-free`, none
// takes a block of data stacks (AcquireStackBlock). It compares the
// code Go makes of each callX function of inlining_test.go with that of
// directX, the call of the function a caller could write by hand: the same
// instructions. inlining_test.go checks what each computes, and that a
// caller's array on its stack stays there where nothing escapes.
import std;

extern fn peek(xs: const i64[:]) -> i64;
extern fn first_byte(text: const u8[:]) -> i64;

struct Pt { x: f64, y: f64 }

// A struct whose Go value is its bytes, which hold the slice's address where
// Go does not see it.
struct Held { n: i64, xs: const i64[:] }

// The sum of xs, wrapping as u64 arithmetic does.
// entry: inline, no escape
export fn sum(xs: u32[:]) -> u64 {
    var s: u64 = 0;
    for x in xs { s += x; }
    s
}

// The bytes of text that are ASCII digits.
// entry: inline, no escape
export fn count_digits(text: const u8[:]) -> i64 {
    var n = 0;
    for c in text {
        if c >= '0' && c <= '9' { n += 1; }
    }
    n
}

// The squared length of p.
// entry: inline, no escape
export fn norm2(p: const Pt&) -> f64 { p.x * p.x + p.y * p.y }

// Adds a * xs into ys, as far as both go.
// entry: inline, no escape
export fn axpy(a: f64, xs: const f64[:], ys: f64[:]) {
    let n = min(xs.len, ys.len);
    for i in n { ys[i] += a * xs[i]; }
}

// The text after its leading spaces: a view of it.
// entry: inline, no escape
export fn trim_left(text: const u8[:]) -> const u8[:] {
    var i = 0;
    while i < text.len && text[i] == ' ' { i += 1; }
    text[i..]
}

// The bytes of a Go []string's strings, which it takes as a
// const (const u8[:])[:] (issue #42), viewed in place.
// entry: inline, no escape
export fn tags_len(tags: const (const u8[:])[:]) -> i64 {
    var n = 0;
    for t in tags { n += t.len; }
    n
}

// Where x is in xs, first and last, and whether once or more than once: a
// struct of two i64s and two bools, 18 bytes in its C layout, which leaves
// Go a tail to pad, so that its Go value is those bytes. Built, read and
// written in a Go local, which none of its addresses outlives an access of,
// it stays a Go local, on Go's stack (issue #62): the function takes no
// frame on the fixed-frame stack, and its entry points no block.
struct Found { first: i64, last: i64, any: bool, many: bool }

// entry: inline, no escape, block-free
export fn find_all(xs: const i64[:], x: i64) -> Found {
    var f = Found { first: -1, last: -1, any: false, many: false };
    for v, i in xs {
        if v == x {
            if f.any { f.many = true; } else { f.first = i; }
            f.last = i;
            f.any = true;
        }
    }
    f
}

// The span of what find_all found, -1 for nothing: a Found as a parameter.
// entry: inline, block-free
export fn found_span(f: Found) -> i64 {
    if !f.any { return -1; }
    f.last - f.first + 1
}

fn weighted(xs: const i64[:], k: i64) -> i64 {
    var s = 0;
    for x, i in xs { s += (i + k) * x; }
    s
}

// The sum of each element weighted by its index and k, through a function
// of its own: Go's analysis follows the slice into it.
// entry: no escape
export fn weighted_sum(xs: const i64[:], k: i64) -> i64 {
    if k < 0 { return weighted(xs[..xs.len / 2], -k); }
    weighted(xs, k)
}

// The slice, kept in a struct.
// entry: escape
export fn held_sum(xs: const i64[:]) -> i64 {
    let h = Held { n: 1, xs: xs };
    var s = h.n;
    for x in h.xs { s += x; }
    s
}

// The slice, kept on a data stack.
// entry: escape
export fn kept_sum(xs: const i64[:]) -> i64 {
    var kept: (const i64[:])[>..] = [];
    kept.push(xs);
    var s = 0;
    for x in kept[0] { s += x; }
    s
}

// The slice, handed to the Go of an extern fn (helper.go).
// entry: escape
export fn peeked(xs: const i64[:]) -> i64 { peek(xs) + 1 }

// The text, handed to the Go of an extern fn, through every entry point.
// entry: escape
export fn first_of(text: const u8[:]) -> i64 { first_byte(text) }
