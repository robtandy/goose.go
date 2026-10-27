// pop_n of more elements than a resizable array holds, or of a negative
// count, aborts (GS_E_POPN).
fn drop(xs: i64[>..<]&, n: i64) { xs.pop_n(n); }

fn main() {
    var g: i64[>..<] = [1, 2, 3, 4];
    drop(g, 3);
    print(g);
    drop(g, 2);
    print("not reached");
}
