// A global whose initializer never completes leaves every initializer after
// it unreached, for Go's flow as for the program, which never gets to main:
// none of them may be written into the Go after the loop (vet reports such
// code), while the data stacks they would take are counted as the C
// backend counts them. The program runs forever, so it is only built.

fn noisy(n: i64) -> i64 {
    print("initializing ", n);
    n * 2
}

let before = noisy(1);
let spin: i64 = loop {};
let after = noisy(2);
var counter = after + 1;

fn main() {
    print(before, " ", spin, " ", after, " ", counter);
}
