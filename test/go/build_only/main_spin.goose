// A main whose loop only an exit leaves: nothing after the loop is reached,
// in Go's flow analysis as in the program. The program runs until it is
// stopped, so it is only built.

fn tick(n: i64) -> i64 {
    print("tick ", n);
    n + 1
}

fn main() {
    var n = 0;
    loop {
        n = tick(n);
        if n < 0 { exit(1); }
    }
    print("never ", n);
    while n > 0 { n -= 1; }
}
