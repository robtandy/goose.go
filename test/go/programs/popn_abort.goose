// pop_n of a negative count, or of more elements than the array holds,
// aborts (GS_E_POPN).
fn drop(l: i64[..8]&, n: i64) { l.pop_n(n); }

fn main() {
    var l: i64[..8] = [1, 2, 3, 4];
    drop(l, 2);
    print(l);
    drop(l, -1);
    print("not reached");
}
