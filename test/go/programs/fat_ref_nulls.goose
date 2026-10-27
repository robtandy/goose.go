// A null reference to a resizable value (a GsRref with no header): a
// default one, and one a frame object's literal leaves out or sets null.
struct F { tag: i64, other: (i64[>..]&)?, items: i64[>..] }
fn main() {
    let d = default<(i64[>..]&)?>();
    let e = default<i64[>..]?>();
    print(d == null, " ", e == null);
    var xs: i64[>..] = [1, 2];
    var f = F { tag: 1, items: [3] };
    print(f.other == null, " ", f.items.len);
    f.other .= xs;
    let o = f.other;
    if o { print(o.len); }
    var g = F { tag: 2, other: null, items: [] };
    print(g.other == null, " ", g.tag);
}
