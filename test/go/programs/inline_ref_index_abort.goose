// An element handed to an inlined body by reference is checked where it is
// bound, as a call checks its argument (issue #24): an index past the
// array's length aborts there, with the index and the length, whether the
// body reads the reference or not, after what was printed before it.
inline fn unread(x: i64&, n: i64) -> i64 { n * 2 }
inline fn bump(x: i64&) { x += 1; }

fn main() {
    var a: i64[4] = [1, 2, 3, 4];
    var t = 0;
    for i in 6 {
        t += unread(a[i], i);
        bump(a[i]);
        print(i, " ", a[i], " ", t);
    }
    print("not reached");
}
