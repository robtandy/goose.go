// Control flow as expressions (§6.4, §6.5): if/else values, guard, block
// with break values, bare blocks, while, loop with a value, for over ranges
// and counts (narrow counters included), break and continue in nested
// loops, and match over integers with values, lists and ranges.

fn classify(n: i64) -> i32 {
    if n < 0 { -1 } else if n == 0 { 0 } else if n < 10 { 1 } else { 2 }
}

fn grade(score: i32) -> i32 {
    match score {
        100 => 6,
        90..100 => 5,
        80..90 => 4,
        70..80, 65 => 3,
        _ => 0,
    }
}

fn half(n: i64) -> i64 {
    guard n % 2 == 0 else { return -1; }
    n / 2
}

fn first_square_over(limit: i64) -> i64 {
    var i = 0;
    let found = loop {
        i += 1;
        if i * i > limit { break i * i; }
    };
    found
}

fn collatz(start: u64) -> i32 {
    var n = start;
    var steps: i32 = 0;
    while n != 1 {
        n = if n % 2 == 0 { n / 2 } else { 3 * n + 1 };
        steps++;
    }
    steps
}

fn main() {
    print(classify(-5), " ", classify(0), " ", classify(7), " ", classify(99));
    print(grade(100), " ", grade(95), " ", grade(85), " ", grade(65), " ", grade(72), " ", grade(10));
    print(half(10), " ", half(7), " ", first_square_over(50), " ", collatz(27));

    // block: an early-out with a value.
    for v in -1..4 {
        let verdict = block {
            guard v >= 0 else { break 100; }
            if v == 0 { break 200; }
            guard v != 2 else { break 300; }
            v * 1000
        };
        print("v=", v, " -> ", verdict);
    }
    // A bare block is a scope; its value is its tail.
    let doubled = { let t = 21; t * 2 };
    print(doubled);

    // Nested loops: break and continue reach the innermost loop.
    var pairs = 0;
    for i in 0..6 {
        if i == 4 { continue; }
        for j in 0..6 {
            if j > i { break; }
            if (i + j) % 3 == 0 { continue; }
            pairs += i * 10 + j;
        }
    }
    print("pairs ", pairs);

    // A loop over a narrow range runs at its type; a binder one past the
    // type's end is a copy of a wider counter.
    var count: u8 = 0;
    for i: u8 in 0..256 { count += 1; }
    var last: i8 = 0;
    for k: i8 in -128..127 { last = k; }
    print(count, " ", last);
    var total: u64 = 0;
    for i in 5 { total += i as u64; }
    for i in 10..13 { total += i as u64; }
    for i in 3..1 { total += 1000; }
    print(total);

    // Loops whose condition calls a function.
    var calls = 0;
    fn more(n: i64) -> bool { n < 4 }
    var w = 0;
    while more(w) { w += 1; calls += 1; }
    print("w ", w, " calls ", calls);

    // if as a statement, nested, with else-if chains.
    for n in 0..16 {
        if n % 15 == 0 {
            print("fizzbuzz");
        } else if n % 5 == 0 {
            print("buzz");
        } else if n % 3 == 0 {
            print("fizz");
        } else {
            print(n);
        }
    }

    // match as a statement, and over u8 and negative values.
    for b: u8 in 250..255 {
        match b {
            250 => print("250"),
            251, 252 => print("251 or 252"),
            _ => print("other ", b),
        }
    }
    let m = match -3 { -5..0 => -50, _ => 50 };
    print(m);

    // Short circuit: the right operand runs only where it decides.
    var hits = 0;
    fn hit(r: bool) -> bool {
        hits++;
        r
    }
    let p = hit(false) && hit(true);
    let q = hit(true) || hit(false);
    let r = hit(true) && hit(false) || hit(true);
    print(p, " ", q, " ", r, " ", hits);
}
