// goose-flags: --stack-reserve 64K
// A resizable array grows with no check of its stack's room: one that grows
// past its region's end runs into the guard gap behind it, which the runtime
// reports as a data stack overflow, naming the reservation, as C's fault
// handler does.
fn main() {
    var a: i64[>..] = [];
    for i in 4000 { a.push(i); }
    print(a.len, " ", a[3999]);
    for i in 10000 { a.push(i); }
    print("not reached");
}
