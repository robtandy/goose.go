// Data stacks (§10.3) and their static count (BoundStacks): print renders
// its line on a stack of its statement, and a call in its arguments is
// handed the index above it, so a line under construction survives callees
// that print lines of their own, however deep; a recursive cycle is called
// with no stack in use. The runner also compares the `--stacks` report of
// this program between the two backends.

fn leaf(n: i64) -> i64 {
    print("    leaf ", n);
    n + 1
}

fn middle(n: i64) -> i64 {
    print("  middle ", n, " sees ", leaf(n * 10), " and ", leaf(n * 100));
    n * 2
}

fn top(n: i64) -> i64 {
    print("top ", n, " -> ", middle(n), " -> ", middle(n + 1));
    n * 3
}

// No data stack: needs no block.
fn quiet(n: i64) -> i64 { n * n }

recursive fn depth(n: i64) -> i64 {
    if n == 0 { return 0; }
    // The line is written before the recursive call, which takes the
    // statement's stack index again.
    print("depth ", n);
    let below = depth(n - 1);
    return below + 1;
}

recursive fn ping(n: i64) -> i64 {
    if n <= 0 { return 0; }
    print("ping ", n);
    return pong(n - 1) + 1;
}

fn pong(n: i64) -> i64 {
    if n <= 0 { return 0; }
    print("pong ", n);
    return ping(n - 1) + 1;
}

fn main() {
    print("result ", top(1), " ", quiet(9));
    print("depth -> ", 0, " ", quiet(3));
    let d = depth(4);
    print("depth -> ", d);
    let p = ping(5);
    print("ping-pong -> ", p, " ", leaf(leaf(leaf(0))));
}
