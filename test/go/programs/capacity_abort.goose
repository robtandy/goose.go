// A push past a limited array's capacity aborts (GS_E_CAPACITY), and so does
// a slice copied into one that it does not fit.
fn fill(l: i64[..3]&, n: i64) { for i in n { l.push(i); } }

fn main() {
    var l: i64[..3] = [];
    fill(l, 3);
    print(l, " ", l.len);
    l.clear();
    fill(l, 2);
    print(l);
    fill(l, 2);
    print("not reached");
}
