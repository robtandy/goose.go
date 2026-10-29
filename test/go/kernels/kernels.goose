// A library of kernels (issue #19): pure functions over slices, which take
// no data stack, hold no global and spawn no worker. Its Go needs nothing
// of the runtime's data stacks, pools or text forms, and test/run_go_tests.py
// checks that the file the compiler writes holds none of them (the runtime's
// functions it names are absent, and the file is short); kernels_test.go
// calls the entry points.

// The dot product of a and b, over the elements they both have.
export fn dot(a: const f64[:], b: const f64[:]) -> f64 {
    var s = 0.0;
    for x, i in a {
        if i < b.len { s += x * b[i]; }
    }
    s
}

// Scales xs in place by k.
export fn scale(xs: f64[:], k: f64) {
    for &x in xs { x *= k; }
}

// The sum of xs, wrapping as i64 arithmetic does.
export fn sum(xs: const i64[:]) -> i64 {
    var s = 0;
    for x in xs { s += x; }
    s
}

// The index of the largest element of xs, the first of equal ones; -1 for
// none.
export fn argmax(xs: const i64[:]) -> i64 {
    guard xs.len > 0 else { return -1; }
    var best = 0;
    for x, i in xs {
        if x > xs[best] { best = i; }
    }
    best
}

// Each prefix sum of xs into out, as far as out goes: how many it wrote.
export fn prefix_sums(xs: const i64[:], out: i64[:]) -> i64 {
    var s = 0;
    var n = 0;
    for x, i in xs {
        if i >= out.len { break; }
        s += x;
        out[i] = s;
        n += 1;
    }
    n
}

// The bytes of text that are ASCII digits.
export fn count_digits(text: const u8[:]) -> i64 {
    var n = 0;
    for c in text {
        if c >= '0' && c <= '9' { n += 1; }
    }
    n
}

// The quotient of a and b: a division by zero aborts, which crosses to the
// Go caller as a panic.
export fn quotient(a: i64, b: i64) -> i64 { a / b }
