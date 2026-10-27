// A non-null target at an optional self-relative slot's own address would
// encode as its null, 0, and aborts (GS_E_RELNULL).
struct Link { to: Link&<u16>?, v: i64 }

fn main() {
    var ls = [Link { to: null, v: 1 }, Link { to: null, v: 2 }];
    ls[0].to .= ls[1];
    let t = ls[0].to;
    guard t else { return; }
    print(t.v);
    ls[1].to .= ls[1];
    print("not reached");
}
