// range_index of a slice that is not wholly in the array, which the checker
// cannot prove, aborts at run time (issue #59): with the message it is
// given, and otherwise the runtime's.
fn locate(a: i64[>..]&, s: i64[:]) -> i64 { a.range_index(s, "locate: not a run of a") }
fn plain(a: i64[>..]&, s: i64[:]) -> i64 { a.range_index(s) }

fn main() {
    var a: i64[>..] = [1, 2, 3];
    var b: i64[>..] = [4, 5];
    print(locate(a, a[1..]), " ", plain(a, b[..0]));
    print(plain(a, b[1..]));
    print("not reached");
}
