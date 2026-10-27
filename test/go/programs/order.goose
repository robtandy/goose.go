// Evaluation order (§2) and the corners of the generated code's shape:
// operands and arguments left to right where a later one has effects,
// short-circuit operands, shadowed parameters, locals assigned on each
// branch before use, inlined calls returning from deep inside, loops with
// a value, matches whose first arm is the wildcard, and functions that end
// in a loop the checker knows never ends.

var trace: i64 = 0;

fn step(tag: i64) -> i64 {
    trace = trace * 10 + tag;
    tag
}

fn sum3(a: i64, b: i64, c: i64) -> i64 { a * 100 + b * 10 + c }

// Inlined, with returns from inside loops and blocks.
fn find_first(limit: i64, div: i64) -> i64 {
    var i = 1;
    while i < limit {
        if i % div == 0 {
            {
                let r = i * 2;
                return r;
            }
        }
        i += 1;
    }
    return -1;
}

fn nested_inline(n: i64) -> i64 {
    find_first(n, 3) + find_first(n, 5) + find_first(2, 7)
}

fn shadow(x: i64) -> i64 {
    let x = x + 1;
    let x = x * 2;
    x
}

fn spin_until(n: i64) -> i64 {
    var i = 0;
    loop {
        i += 1;
        if i * i >= n { return i; }
    }
}

fn pick(c: i64) -> i64 {
    match c {
        _ => c * 7,
    }
}

fn main() {
    // Operands: the left one is read before the right one's call runs.
    var x = 5;
    fn bump() -> i64 {
        x += 100;
        x
    }
    let y = x + bump();
    print(y, " ", x);
    let z = bump() + x;
    print(z, " ", x);
    // Arguments.
    trace = 0;
    print(sum3(step(1), step(2), step(3)), " ", trace);
    trace = 0;
    let w = sum3(x, bump(), x);
    print(w, " ", x);
    // Short circuit.
    trace = 0;
    let b1 = step(0) == 1 && step(5) == 5;
    let b2 = step(1) == 1 || step(6) == 6;
    let b3 = step(2) == 2 && (step(3) == 0 || step(4) == 4);
    print(b1, " ", b2, " ", b3, " ", trace);
    // Compound assignment reads its target first.
    var acc = 10;
    fn twice_acc() -> i64 {
        acc *= 2;
        acc
    }
    acc += twice_acc();
    print(acc);

    print(nested_inline(20), " ", shadow(4), " ", spin_until(50), " ", pick(6));

    // A local assigned on every path before it is read.
    var v: i64;
    if x > 1000 { v = 1; } else if x > 100 { v = 2; } else { v = 3; }
    print(v);

    // A loop's value from breaks at different depths.
    var k = 0;
    let found = loop {
        k += 1;
        if k > 100 { break -1; }
        if k % 7 == 0 {
            if k % 2 == 0 { break k * 10; }
        }
    };
    print(found, " ", k);

    // Self-assignment, and a value nobody reads.
    var same = 3;
    same = same;
    same + 1;
    print(same);
}
