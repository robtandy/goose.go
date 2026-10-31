// move_from of slices of different lengths aborts with its message (issue
// #59), after the program's own output, the receiver's elements as they
// were.
fn main() {
    var a: i64[>..] = [1, 2, 3, 4];
    a[..2].move_from(a[2..]);
    print(a);
    a[..3].move_from(a[2..]);
    print("not reached");
}
