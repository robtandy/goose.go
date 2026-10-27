// resize past the length without a fill value aborts (GS_E_RESIZEFILL),
// as does one past the capacity (GS_E_CAPACITY) or below zero.
fn shrink(l: i64[..8]&, n: i64) { l.resize(n); }

fn main() {
    var l: i64[..8] = [1, 2, 3, 4];
    shrink(l, 2);
    print(l);
    l.resize(5, 7);
    print(l);
    shrink(l, 6);
    print("not reached");
}
