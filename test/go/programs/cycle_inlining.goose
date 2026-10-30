// Recursive fns calling helpers that are in no recursive cycle (issue #42):
// under --target=go each helper whose code takes no data stack is inlined
// into a function of a cycle as into any other function, an inline fn
// included, where the optimizer inlines nothing into one. A helper that takes
// a data stack -- a print, a str, a value of more than 4096 bytes, which C
// holds on one -- stays a call, as in C, and so does one calling into a
// cycle: each activation holds the stacks it does in C, and the two backends
// report the same (--stacks). run_go_tests.py checks at -O2 that the Go
// function of each function an `// inlined:` line annotates calls none of
// the Go functions of the helpers it names, and that it calls each one its
// `// calls:` line names (Runner.cycles).
import std;

inline fn mix(h: u64, x: u64) -> u64 { (h ^ x) * 1099511628211 }

fn square(x: i64) -> i64 { x * x }

// An early return.
fn floor0(x: i64) -> i64 {
    if x < 0 { return 0; }
    x
}

// A loop, and a single use.
fn digits(n: i64) -> i64 {
    var k = 1;
    var m = n;
    while m >= 10 {
        m /= 10;
        k += 1;
    }
    k
}

fn weight(d: i64) -> i64 {
    if d % 2 == 0 { return d; }
    10 - d
}

// A local handed to it by reference.
inline fn bump(c: i64&, by: i64) { c += by; }

// Takes a data stack: the line print renders there.
fn show(depth: i64, x: i64) {
    print("  depth ", depth, ": ", x);
}

// Takes a data stack: the text str builds there.
fn width(x: i64) -> i64 { str(x).len }

// A local of more than 4096 bytes, which C holds on a data stack outside a
// recursive cycle.
fn spread(x: i64) -> i64 {
    var a: i64[600] = [0; 600];
    a[x % 600] = x;
    var s = 0;
    for v in a { s += v; }
    s
}

// Calls into a cycle of its own.
fn fib_mod(n: i64) -> i64 { fib(n) % 7 }

// Halves of a range, a leaf's helpers at the bottom.
// inlined: mix, square, floor0, max
// calls: show, width, fib_mod
recursive fn tree(lo: i64, hi: i64, depth: i64) -> u64 {
    if hi - lo <= 1 {
        if hi == lo { return 0; }
        return mix(1, max(square(floor0(lo - 3)), lo) as! u64);
    }
    if depth == 1 { show(depth, hi - lo + width(lo) + fib_mod(depth + 5)); }
    let mid = (lo + hi) / 2;
    mix(tree(lo, mid, depth + 1), tree(mid, hi, depth + 1))
}

// A base case the optimizer copies into the self-calls (optimize_basecase.h),
// a helper's call in it.
// inlined: floor0, square
recursive fn fib(n: i64) -> i64 {
    if n < 2 { return floor0(n); }
    fib(n - 1) + square(fib(n - 2)) % 1000
}

// A tail sum the optimizer turns into a loop (optimize_tre.h), its terms a
// helper's.
// inlined: square
recursive fn sum_squares(n: i64) -> i64 {
    if n == 0 { return 0; }
    square(n) + sum_squares(n - 1)
}

// A cycle of two functions, the digits of n weighed alternately.
// inlined: weight
recursive fn even_turn(n: i64, acc: i64) -> i64 {
    if n == 0 { return acc; }
    odd_turn(n / 10, acc + weight(n % 10))
}

// inlined: weight
fn odd_turn(n: i64, acc: i64) -> i64 {
    if n == 0 { return -acc; }
    even_turn(n / 10, acc + 2 * weight(n % 10))
}

// A recursive fn that does not recurse, as code once marked one to keep it
// from being dropped.
// inlined: mix, digits
recursive fn once(n: i64) -> u64 { mix(digits(n) as! u64, n as! u64) }

// A local of the recursive fn handed to an inlined helper by reference.
// inlined: bump
recursive fn count_nodes(lo: i64, hi: i64) -> i64 {
    var c = 0;
    bump(c, 1);
    if hi - lo > 1 {
        let mid = (lo + hi) / 2;
        bump(c, count_nodes(lo, mid));
        bump(c, count_nodes(mid, hi));
    }
    c
}

// calls: spread
recursive fn spread_sum(n: i64) -> i64 {
    if n == 0 { return 0; }
    spread(n * 7) + spread_sum(n - 1)
}

// A nested recursive fn, reading a local of the function it is in.
fn nested_sum(n: i64) -> i64 {
    let k = n % 3 + 1;
    // inlined: square
    recursive fn walk(i: i64) -> i64 {
        if i == 0 { return 0; }
        square(k + i % 2) + walk(i - 1)
    }
    walk(n)
}

fn main() {
    print("tree ", tree(0, 37, 0));
    print("fib ", fib(20), " ", fib(1), " ", fib(0));
    print("sum_squares ", sum_squares(100));
    print("even_turn ", even_turn(9876543210, 0), " ", even_turn(123, 0));
    print("once ", once(4096));
    print("count_nodes ", count_nodes(0, 100));
    print("spread_sum ", spread_sum(5));
    print("nested_sum ", nested_sum(10), " ", nested_sum(11));
}
