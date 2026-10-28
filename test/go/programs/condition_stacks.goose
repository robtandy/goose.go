// The operands of a && or || that decides a branch -- an if's, a guard's, a
// while's or an assert's condition, nested and negated, and an inlined bool
// function's returned value there -- each give back the data stacks their
// temporaries took once their truth is taken, as C's branches do, so that
// both backends take the same stacks (--stacks). As a value, a && or ||
// holds its left operand's temporaries while its right one runs, in both.
fn word(long: bool) -> u8[] {
    if long { return "abcdef"; }
    return "ab";
}

fn named_word(xs: u8[:], n: i64) -> bool {
    let w = str("w", n);
    if w == xs { return true; }
    return word(n > 4) == xs || str(n) == xs;
}

fn conditions(xs: u8[:], n: i64) -> i64 {
    var r = 0;
    if word(n > 3) == xs || str(n) == xs { r += 1; }
    if !(str(n) == "3" && word(n > 1) != xs) { r += 10; }
    if named_word(xs, n) { r += 100; } else { r += 1000; }
    assert(str(n) != xs || word(n > 4) != "xyz");
    guard word(n > 4) != xs || xs.len > 5 && str(n) != "6" else { return r + 10000; }
    var i = 0;
    while i < n && (word(i > 3) != "abcdef" || str(i) != "6") { i++; }
    let v = str(n) == "5" || word(n > 2) == xs;
    return r + i * 100000 + (if v { 1000000 } else { 0 });
}

fn main() {
    for n in 8 {
        print(conditions("ab", n), " ", conditions("abcdef", n), " ", conditions("5", n), " ",
              conditions("w7", n));
    }
}
