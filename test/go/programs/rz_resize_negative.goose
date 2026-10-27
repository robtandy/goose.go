// resize of a resizable array to a negative length aborts (GS_E_RESIZENEG).
fn size(xs: i64[>..<]&, n: i64) { xs.resize(n, 0); }

fn main() {
    var g: i64[>..<] = [1, 2];
    size(g, 3);
    print(g);
    size(g, -1);
    print("not reached");
}
