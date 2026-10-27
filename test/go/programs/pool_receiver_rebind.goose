// A pool receiver reached through a reference its argument rebinds is
// resolved, whole, before the argument runs (§2): the slot comes from the
// pool the reference named then, elements and freelist alike.
struct Cell { v: i64 }
fn main() {
    reusable var a: Cell[>..] = [];
    reusable var b: Cell[>..] = [];
    let a0 = a.alloc_index(Cell { v: 1 });
    b.alloc_index(Cell { v: 10 });
    b.alloc_index(Cell { v: 20 });
    b.alloc_index(Cell { v: 30 });
    b.free(1);
    var r: Cell[>..]& = a;
    fn swap() -> Cell { r .= b; return Cell { v: 99 }; }
    let i = r.alloc_index(swap());
    print(a0, " ", i, " ", a.len, " ", b.len);
    var t = 0;
    for x in a { t = t * 100 + x.v; }
    print(t);
    let j = r.alloc_index(Cell { v: 7 });
    print(j, " ", b.len, " ", b[j].v);
}
