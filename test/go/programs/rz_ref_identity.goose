// Identity of references to resizable values (§3.8): the same array is
// the same header, whatever the reference's path.
fn same(a: i64[>..]&, b: i64[>..]&) -> bool { return a .== b; }
fn other(a: i64[>..]&, b: i64[>..]&) -> bool { return a .!= b; }
struct H { xs: i64[>..] }
struct Cell { v: i64 }
fn same_pool(a: Cell[>..]&, b: Cell[>..]&) -> bool { return a .== b; }
// Pool references compare as the pools' headers too.
fn pools() {
    reusable var a: Cell[>..] = [];
    reusable var b: Cell[>..] = [];
    var r: Cell[>..]& = a;
    var s: Cell[>..]& = a;
    print(r .== s, " ", same_pool(r, s), " ", same_pool(a, b));
    s .= b;
    print(r .== s, " ", r .!= s, " ", same_pool(r, s));
}
fn main() {
    var x: i64[>..] = [1];
    var y: i64[>..] = [1];
    print(same(x, x), " ", same(x, y), " ", other(x, y), " ", other(y, y));
    var r .= x;
    var s .= x;
    print(r .== s, " ", r .!= s);
    s .= y;
    print(r .== s, " ", r .!= s);
    var o: (i64[>..]&)? = null;
    print(o .== null, " ", o == null);
    o .= y;
    print(o .== s, " ", o .== r);
    pools();
}
