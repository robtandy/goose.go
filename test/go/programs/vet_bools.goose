// Conditions Go's vet takes for mistakes, which Goose takes as written: an
// operand twice in one chain of `&&`s or `||`s, through parentheses, and one
// value compared with two constants (vet's bools analyzer). The Go must vet
// clean all the same, and short-circuit as the program does.

var calls: i64 = 0;

fn tick(c: bool) -> bool {
    calls += 1;
    c
}

fn twice_and(c: bool) -> bool { c && c }
fn twice_or(c: bool) -> bool { c || c }
fn not_twice(c: bool) -> bool { !c || !c }
fn cmp_twice(n: i64, c: bool) -> bool { (n < 3) && c && (n < 3) }
fn always(n: i64) -> bool { n != 1 || n != 2 }
fn never(n: i64) -> bool { n == 1 && n == 2 }
fn nested(c: bool, d: bool) -> bool { (c && d) || (c && d) || c }
fn mixed(n: i64, c: bool) -> bool { (n == 0 || c) && (n == 0 || c) }

// Calls run where the left operand leaves the value open, each time.
fn calls_twice(c: bool) -> bool { tick(c) && tick(c) }

fn main() {
    print(twice_and(true), " ", twice_and(false), " ", twice_or(true), " ", twice_or(false));
    print(not_twice(true), " ", not_twice(false), " ", cmp_twice(1, true), " ", cmp_twice(5, true));
    print(always(1), " ", always(2), " ", never(1), " ", never(2));
    print(nested(true, true), " ", nested(true, false), " ", nested(false, true));
    print(mixed(0, false), " ", mixed(1, false), " ", mixed(1, true));
    let first = calls_twice(true);
    let after_first = calls;
    let second = calls_twice(false);
    print(first, " ", after_first, " ", second, " ", calls);
}
