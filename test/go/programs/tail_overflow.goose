// goose-flags: --debug
// The sum 127 + (1 + (-1 + 0)) of i8s, as the recursion adds it, does not
// overflow; at -O1 and up, tail-recursion elimination makes it a loop that
// adds 127, then 1, which does (optimize_tre.h: --debug checks the
// operations as they run). The runner compares each Go build with the C
// backend's at the same level: this prints 127 at -O0 and fails at -O2.

fn term(i: i64) -> i8 {
    if i == 0 { 127 } else if i == 1 { 1 } else { -1 }
}

recursive fn sum(i: i64) -> i8 {
    let t = term(i);
    if i >= 3 { return 0; }
    return t + sum(i + 1);
}

fn main() {
    print(sum(0));
}
