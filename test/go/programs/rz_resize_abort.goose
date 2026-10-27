// resize of a resizable array past its length without a fill value aborts
// (GS_E_RESIZEFILL); one below zero would too (GS_E_RESIZENEG).
fn size(xs: i64[>..<]&, n: i64) { xs.resize(n); }

fn main() {
    var g: i64[>..<] = [1, 2, 3, 4];
    size(g, 2);
    print(g);
    g.resize(4, 9);
    print(g);
    size(g, 6);
    print("not reached");
}
