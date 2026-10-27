// An index past an array's length aborts with the index and the length, as
// the C runtime's GS_IDX does, after what was printed before it.
fn at(a: i64[4], i: i64) -> i64 { a[i] }

fn main() {
    let a = [1, 2, 3, 4];
    print(at(a, 3));
    var s: i64[:] = a;
    print(s[0], " ", at(a, 0));
    print(at(a, 4));
    print("not reached");
}
