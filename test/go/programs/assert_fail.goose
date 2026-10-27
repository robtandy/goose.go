// A failed assert (§9.3) ends the program with a message naming its file
// and line on stderr and exit status 1, after what was printed before it.

fn check(n: i64) -> i64 {
    assert(n % 7 != 0);
    n * 2
}

fn main() {
    var total = 0;
    for i in 1..10 {
        total += check(i);
        print("checked ", i, " total ", total);
    }
    print("not reached");
}
