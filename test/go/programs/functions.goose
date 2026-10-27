// Functions (§7): direct calls, recursion (one function and a mutual
// cycle), tail recursion the optimizer turns into a loop, several results,
// default parameters, overloads, generics, UFCS, nested functions that read
// and write the enclosing function's locals, and function values.

recursive fn fib(n: i64) -> i64 {
    if n < 2 { return n; }
    return fib(n - 1) + fib(n - 2);
}

recursive fn ackermann(m: i64, n: i64) -> i64 {
    if m == 0 { return n + 1; }
    if n == 0 { return ackermann(m - 1, 1); }
    return ackermann(m - 1, ackermann(m, n - 1));
}

// A mutual cycle.
recursive fn is_even(n: u32) -> bool {
    if n == 0 { return true; }
    return is_odd(n - 1);
}
fn is_odd(n: u32) -> bool {
    if n == 0 { return false; }
    return is_even(n - 1);
}

// Accumulator tail recursion, which the optimizer turns into a loop (a
// leading local keeps base-case inlining from rewriting the self-calls
// first). The depths stay within the C backend's native stack, which an
// -O0 build, without the loops, recurses on.
recursive fn sum_to(n: i64) -> i64 {
    let before_basecase = 0;
    if n == 0 { return 0; }
    return n + sum_to(n - 1);
}
recursive fn gcd(a: u64, b: u64) -> u64 {
    let before_basecase = 0;
    if b == 0 { return a; }
    return gcd(b, a % b);
}
recursive fn power(base: i64, e: i64) -> i64 {
    let before_basecase = 0;
    if e == 0 { return 1; }
    return base * power(base, e - 1);
}
// Plain tail recursion: both parameters change, the new k reading the old n.
recursive fn count_down(n: i64, k: i64) -> i64 {
    let before_basecase = 0;
    if n <= 0 { return k; }
    return count_down(n - 1, k + n);
}
// Two operators: the first tail becomes the loop, the other stays a call.
recursive fn mixed(n: i64) -> i64 {
    let before_basecase = 0;
    if n <= 0 { return 1; }
    if n % 2 == 0 { return 2 + mixed(n - 1); }
    return 3 * mixed(n - 1);
}
// `&` over u64: the accumulator starts at all ones.
recursive fn low_bits(n: i64) -> u64 {
    let before_basecase = 0;
    if n <= 0 { return 18446744073709551615; }
    return low_bits(n - 1) & 0xFFFF;
}
// Floats are left alone, since regrouping them changes the answer.
recursive fn halves(n: i64, x: f64) -> f64 {
    let before_basecase = 0;
    if n <= 0 { return x; }
    return 0.5 + halves(n - 1, x);
}

fn divmod(a: i64, b: i64) -> i64, i64, bool {
    if b == 0 { return 0, 0, false; }
    return a / b, a % b, true;
}

// Forwarding every result of a call.
fn divmod10(a: i64) -> i64, i64, bool { return divmod(a, 10); }

fn clamp(x: i64, lo: i64 = 0, hi: i64 = 100) -> i64 {
    if x < lo { lo } else if x > hi { hi } else { x }
}

fn describe(x: i64) -> i64 { x * 2 }
fn describe(x: f64) -> f64 { x / 2.0 }
fn describe(x: bool) -> bool { !x }

fn twice(x) { x + x }
fn span<T>(a: T, b: T) -> T { if a > b { a - b } else { b - a } }
fn zero<T>() -> T { default<T>() }

fn apply<F>(x: i64) -> i64 { F(x) }
fn repeat<F>(n: i64) {
    var i = 0;
    while i < n { F(i); i += 1; }
}

var calls: i64 = 0;
fn counted(x: i64) -> i64 {
    calls += 1;
    x * x
}

fn main() {
    print(fib(20), " ", ackermann(2, 3), " ", ackermann(3, 3));
    print(is_even(10), " ", is_even(7), " ", is_odd(9));
    print(sum_to(50000), " ", gcd(1071, 462), " ", gcd(17, 5), " ", power(3, 13));
    print(count_down(50000, 7), " ", mixed(9), " ", low_bits(3), " ", halves(3, 0.125));

    let q, r, ok = divmod(47, 5);
    print(q, " ", r, " ", ok);
    let q2, r2, ok2 = divmod(1, 0);
    print(q2, " ", r2, " ", ok2);
    var a = 0;
    var b = 0;
    var c = false;
    a, b, c = divmod10(-123);
    print(a, " ", b, " ", c, " ", divmod(9, 4));

    print(clamp(250), " ", clamp(-5, -10), " ", clamp(50, 0, 10), " ", 7.clamp(0, 5));
    print(describe(21), " ", describe(5.0), " ", describe(false), " ", 3.describe());
    print(twice(21), " ", twice(1.5), " ", twice(100 as u8), " ", span(3, 8), " ", span(2.5, 1.0));
    print(zero<i32>(), " ", zero<f64>() + 0.5, " ", zero<bool>());

    // Arguments evaluate left to right, each call once.
    print(counted(2) + counted(3) * counted(4), " ", calls);

    // Nested functions read and write the enclosing locals, and call each
    // other.
    var total = 0;
    var seen = 0;
    fn add(x: i64) {
        total += x;
        seen++;
    }
    fn add_twice(x: i64) {
        add(x);
        add(x);
    }
    for i in 1..5 { add(i); }
    add_twice(100);
    print("total ", total, " seen ", seen);

    // Function values: a trailing block, named parameters, a named function.
    print(apply(5) { it * 3 }, " ", apply(5) { v => v - 1 }, " ", apply(4, counted));
    var squares = 0;
    repeat(5) { squares += it * it; };
    print("squares ", squares, " calls ", calls);

    // A nested function that is recursive, over a captured local.
    var depth = 0;
    recursive fn down(n: i64) {
        if n == 0 { return; }
        depth++;
        down(n - 1);
    }
    down(7);
    print("depth ", depth);
}
