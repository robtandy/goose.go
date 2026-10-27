// abort(str(...)): the message built on a data stack, with what the
// program printed before it out first (issue #5).
fn find(xs: const i64[:], want: i64) -> i64 {
    for x, i in xs {
        if x == want { return i; }
    }
    abort(str("no ", want, " among ", xs.len, " values"));
}

fn main() {
    let xs = [4, 8, 15, 16, 23, 42];
    print(find(xs, 15));
    print(find(xs, 99));
}
