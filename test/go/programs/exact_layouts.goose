// Fixed-size values hold their C layout byte for byte (§3.2), references
// and slices among their fields too, wherever C packs them unaligned: a
// self-relative field reads back from the offset C gives it, a narrow
// relative link spans the elements C's sizes put within its range, and an
// ADT's payloads overlap behind its tag. Values holding references live in
// data stacks at packed addresses, a reference to a local is stored in a
// value's bytes, and locals whose address is taken stay put across calls,
// recursion and loop iterations.

struct Node { tag: u8, label: const u8[:], back: Node&<u32> }
struct Link { val: i64, label: const u8[:], next: Link&<u8>? }
struct Cell { v: i64 }
struct Holder { k: u8, cell: Cell&, name: const u8[:] }
enum Shape { Dot, Seg { from: Cell&, to: Cell& }, Name { s: const u8[:], n: u8 } }

fn total(hs: Holder[:]) -> i64 {
    var t = 0;
    for h in hs { t += h.cell.v * 10 + h.name.len + h.k; }
    return t;
}

fn measure(s: Shape) -> i64 {
    match s {
        Dot => 0,
        Seg g => g.to.v - g.from.v,
        Name m => m.s.len * 100 + m.n,
    }
}

recursive fn depth(n: i64, up: Cell&) -> i64 {
    var mine = Cell { v: n * 3 };
    if n == 0 { return up.v; }
    let below = depth(n - 1, mine);
    // The callee's frames came and went above this one: mine is intact,
    // and the caller's, which up points at.
    return below * 10 + mine.v + up.v % 7;
}

fn main() {
    // self, at C's offset of a field after a u8 and a slice.
    let n = Node { tag: 42, label: "x", back: self };
    let b .= n.back;
    print(b.tag == n.tag, " ", b.tag, " ", b.label);           // true 42 x

    // A <u8> link spans elements of C's size: 0 to 5 is 5 * 25 bytes.
    var ls: Link[6] = [Link { val: 0, label: "a", next: null }; 6];
    for i in 6 { ls[i].val = i * 10; }
    ls[0].next .= ls[5];
    let l = ls[0].next;
    if l { print(l.val, " ", l.label); }                          // 50 a

    // References to locals in values' bytes, on a data stack, packed.
    var c1 = Cell { v: 3 };
    var c2 = Cell { v: 4 };
    var hs: Holder[>..] = [];
    hs.push(Holder { k: 1, cell: c1, name: "one" });
    hs.push(Holder { k: 2, cell: c2, name: "two!" });
    hs.push(Holder { k: 3, cell: c1, name: "" });
    c1.v = 30;
    print(total(hs), " ", hs.len);                                 // 653 3
    for h in hs { print(h.k, " ", h.cell.v, " ", h.name); }

    // ADT payloads overlap behind the tag, references and slices too.
    var a = Cell { v: 5 };
    var z = Cell { v: 12 };
    let shapes: Shape[3] = [Shape.Dot, Shape.Seg { from: a, to: z }, Shape.Name { s: "abc", n: 7 }];
    var m = 0;
    for s in shapes { m = m * 1000 + measure(s); }
    print(m);                                                      // 7307
    var grid: Shape[>..] = [];
    for i in 4 {
        if i % 2 == 0 { grid.push(Shape.Seg { from: a, to: z }); }
        else { grid.push(Shape.Name { s: "hi", n: i as u8 }); }
    }
    z.v = 100;
    m = 0;
    for s in grid { m += measure(s); }
    print(m);                                                      // 594

    // A reference to a slice, and slices in a limited array.
    var words: (const u8[:])[..4] = [];
    words.push("ab");
    words.push("cde");
    var w: (const u8[:])& = words[1];
    print(w.len, " ", words.len);                                  // 3 2
    w .= words[0];
    print(w);                                                      // ab

    // Frames across recursion and loop iterations.
    var root = Cell { v: 1000 };
    print(depth(5, root));                                         // 399551
    var sum = 0;
    for i in 3 {
        var local = Cell { v: i };
        let h = Holder { k: 0, cell: local, name: "l" };
        local.v += 10;
        sum += h.cell.v;
    }
    print(sum);                                                    // 33
}
