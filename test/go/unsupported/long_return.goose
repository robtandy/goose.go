// `return ... from` through a frame of its own comes with a later version
// of the Go backend (a recursive function is never inlined, so the return
// crosses its call).
recursive fn find(n: i64) -> i64 {
    if n > 3 { return n from search; }
    if n > 100 { return find(n - 1); }
    0
}
fn search(limit: i64) -> i64 {
    for i in 0..limit { find(i); }
    return -1;
}
fn main() { print(search(10)); }
// error: `return ... from` (a long-distance return) is not supported by --target=go yet
